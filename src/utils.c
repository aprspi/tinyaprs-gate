/*
 * utils.c
 *
 *  Created on: 2016年8月29日
 *      Author: shawn
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <sys/uio.h>
#include <time.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/time.h>

#include <ctype.h>
#include <strings.h>
#include <sys/types.h>
#include <netdb.h>

#include "hash.h"

#include "utils.h"

int resolve_host(const char *hostname_port_pair /*rotate.aprs2.net:14580*/,
		struct sockaddr_inx *sa) {
	struct addrinfo hints, *result;
	char host[51] = "", s_port[10] = "";
	int port = 0, rc;

	if (hostname_port_pair == NULL) {
		return -EINVAL;
	}

	if (sscanf(hostname_port_pair, "%50[^:]:%d", host, &port) == 0) {
		return -EINVAL;
	}
	if (port == 0)
		port = 14580;
	sprintf(s_port, "%d", port);
	if (port <= 0 || port > 65535)
		return -EINVAL;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET; /* Allow IPv4 only */
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE; /* For wildcard IP address */
	hints.ai_protocol = 0; /* IPPROTO_TCP */
	hints.ai_canonname = NULL;
	hints.ai_addr = NULL;
	hints.ai_next = NULL;

	if ((rc = getaddrinfo(host, s_port, &hints, &result)))
		return -EAGAIN;

	/* Get the first resolution. */
	memcpy(sa, result->ai_addr, result->ai_addrlen);

	freeaddrinfo(result);

	char s_server_addr[50];
	inet_ntop(sa->sa.sa_family, addr_of_sockaddr(sa), s_server_addr,
			sizeof(s_server_addr));
	DBG("Resolved to %s:%u", s_server_addr, ntohs(port_of_sockaddr(sa)));

	return 0;
}

int do_daemonize(void) {
	pid_t pid;

	if ((pid = fork()) < 0) {
		fprintf(stderr, "*** fork() error: %s.\n", strerror(errno));
		return -1;
	} else if (pid > 0) {
		/* In parent process */
		exit(0);
	} else {
		/* In child process */
		int fd;
		setsid();
		chdir("/tmp");
		if ((fd = open("/dev/null", O_RDWR)) >= 0) {
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
			if (fd > 2)
				close(fd);
		}
	}
	return 0;
}

time_t get_time_milli_seconds() {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	time_t time_in_mill = (tv.tv_sec) * 1000 + (tv.tv_usec) / 1000;
	return time_in_mill;
}

void hexdump(void *d, size_t len) {
	unsigned char *s;
	uint16_t i = 0;
	printf("=======================================================================\n");
	for (s = d; len; len--, s++){
		printf("%02x ", *s);
		i++;
		if(i > 0){
			if(i % 16 == 0)
				printf("\n");
			else if(i % 8 == 0)
				printf("\t");
		}
	}
	printf("\n^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n");
}

void stringdump(void *d, size_t len) {
	unsigned char *s;
	printf(
			"=======================================================================\n");
	for (s = d; len; len--, s++)
		printf("%c", *s);
	printf(
			"\n^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n");
}

//////////////////////////////////////////////////////////////////
// Simple poll wrapper

#define pollfds_len 8
static int pollfds[pollfds_len];
static poll_callback pollcbs[pollfds_len];
static int maxfd = -1;

int poll_init() {
	int i = 0;
	for (i = 0; i < pollfds_len; i++) {
		pollfds[i] = -1;
		pollcbs[i] = 0;
	}
	return 0;
}

int poll_add(int fd, poll_callback callback) {
	int i = 0;
	for (i = 0; i < pollfds_len; i++) {
		if (pollfds[i] == fd) {
			// all ready there,
			return i;
		} else if (pollfds[i] < 0) {
			pollfds[i] = fd;
			pollcbs[i] = callback;
			if (fd > maxfd) {
				maxfd = fd;
			}
			DBG("Add fd %d to poll list, maxfd is %d", fd, maxfd);
			return i;
		}
	}
	return -1;
}

int poll_remove(int fd) {
	int i = 0, j = 0;
	for (i = 0; i < pollfds_len; i++) {
		if (pollfds[i] == fd) {
			pollfds[i] = -1;
			pollcbs[i] = 0;
			if (maxfd == fd) {
				maxfd = 0;
				// get the maxfd
				for (j = 0; j < pollfds_len; j++) {
					if (pollfds[j] > maxfd) {
						maxfd = pollfds[j];
					}
				}
			}
			DBG("Remove fd %d from poll list, maxfd is %d", fd, maxfd);
			return i;
		}
	}

	return -1;
}

int poll_run() {
	fd_set rset, wset, eset;
	struct timeval timeo;
	int rc;
	int i;

	FD_ZERO(&rset);
	FD_ZERO(&wset);
	FD_ZERO(&eset);

	for (i = 0; i < pollfds_len; i++) {
		if (pollfds[i] >= 0) {
			FD_SET(pollfds[i], &rset);
			FD_SET(pollfds[i], &wset);
			FD_SET(pollfds[i], &eset);
		}
	}

	timeo.tv_sec = 0;
	timeo.tv_usec = 500000; // 500ms

	rc = select(maxfd + 1, &rset, &wset, &eset, &timeo);
	if (rc < 0) {
		fprintf(stderr, "*** select(): %s.\n", strerror(errno));
		return -1;
	} else if (rc > 0) {
		// got ready
		for (i = 0; i < pollfds_len; i++) {
			if (pollfds[i] >= 0 && FD_ISSET(pollfds[i], &rset)
					&& pollcbs[i] > 0) {
				pollcbs[i](pollfds[i], poll_state_read);
			}
			if (pollfds[i] >= 0 && FD_ISSET(pollfds[i], &wset)
					&& pollcbs[i] > 0) {
				pollcbs[i](pollfds[i], poll_state_write);
			}
			if (pollfds[i] >= 0 && FD_ISSET(pollfds[i], &eset)
					&& pollcbs[i] > 0) {
				pollcbs[i](pollfds[i], poll_state_error);
			}

		}
	} else {
		// idle
		for (i = 0; i < pollfds_len; i++) {
			if (pollfds[i] >= 0 && pollcbs[i] > 0) {
				pollcbs[i](pollfds[i], poll_state_idle);
			}
		}
	}

	usleep(50000); // force sleep 50ms as write selet is always returns true.
	return 0;
}

/////////////////////////////////////////////////////////////////////////
// IO Kit

static int io_run(struct IOReader *reader);
static int io_close(struct IOReader *reader);

static int io_readline(struct IOReader *reader) {
	// read data into buffer and callback when CR or LF is met
	if (reader->fd < 0)
		return -1;
	bool flush = false;
	int rc = 0;
	char c = 0;
	while ((rc = read(reader->fd, &c, 1)) > 0) {
		if (c == '\r' || c == '\n') {
			flush = true;
		} else {
			flush = false;
			reader->buffer[reader->bufferLen] = c;
			reader->bufferLen++;
			if (reader->bufferLen == (reader->maxBufferLen - 1)) {
				DBG("read buffer full!");
				// we're full!
				reader->buffer[reader->bufferLen] = 0;
				reader->bufferLen--; // not including the \0
				flush = true;
			}
		}
		if (flush && reader->bufferLen > 0 && reader->callback) {
			reader->buffer[reader->bufferLen] = 0;
			reader->callback(reader->buffer, reader->bufferLen);
			reader->bufferLen = 0;
			flush = false;
		}
	}

	if (rc < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			rc = 0;
		} else {
			ERROR("*** io_readline read() error %d: %s", rc, strerror(errno));
		}
	}

	return rc;
}

static int io_flush(struct IOReader *reader) {
	if (reader->bufferLen > 0 && reader->callback > 0) {
		reader->buffer[reader->bufferLen] = 0;
		reader->callback(reader->buffer, reader->bufferLen);
		reader->bufferLen = 0;
	}
	return 0;
}

static int io_readtimeout(struct IOReader *reader) {
	if (reader->fd < 0)
		return -1;

	int bytesRead =
			read(reader->fd, (reader->buffer + reader->bufferLen),
					(reader->maxBufferLen - reader->bufferLen - 1) /*buffer available*/);
	if (bytesRead > 0) {
		reader->bufferLen += bytesRead;
		reader->lastRead = get_time_milli_seconds();

		// flush buffer if full or wait timeout
		if (reader->bufferLen == (reader->maxBufferLen - 1)) {
			io_flush(reader);
		}
	}

	if (bytesRead < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			bytesRead = 0;
		} else {
			ERROR("*** io_readtimeout read() %d, error: %s", bytesRead,
					strerror(errno));
		}
	}
	return bytesRead;
}

void io_init_linereader(struct IOReader *reader, int fd, uint8_t* buffer,
		size_t bufferLen, void* readercb) {
	bzero(reader, sizeof(struct IOReader));
	reader->fd = fd;
	reader->buffer = buffer;
	reader->maxBufferLen = bufferLen;
	reader->fnRead = io_readline;
	reader->fnRun = io_run;
	reader->fnFlush = io_flush;
	reader->fnClose = io_close;
	reader->callback = readercb;
}

void io_init_timeoutreader(struct IOReader *reader, int fd, uint8_t* buffer,
		size_t bufferLen, int timeout, void* readercb) {
	bzero(reader, sizeof(struct IOReader));
	reader->fd = fd;
	reader->buffer = buffer;
	reader->maxBufferLen = bufferLen;
	reader->fnRead = io_readtimeout;
	reader->fnRun = io_run;
	reader->fnFlush = io_flush;
	reader->fnClose = io_close;
	reader->callback = readercb;
	reader->timeout = timeout;
}

static int io_run(struct IOReader *reader) {
	if (reader->timeout > 0) {
		size_t t = get_time_milli_seconds();
		if (t - reader->lastRead > reader->timeout) {
			io_flush(reader);
		}
	}
	return 0;
}

static int io_close(struct IOReader *reader) {
//	if(reader->fd >=0){
//		close(reader->fd);
//	}
	bzero(reader, sizeof(struct IOReader));
	reader->fd = -1;
	return 0;
}

static inline uint32_t trunc(double dbl){
	return (uint32_t)(dbl * 1000000) / 1000000;
}

/*
 *  split the "LAT,LON"
 */
static bool parse_location_arg(char* latlon, char* lato, size_t latoLen,
		char* lono, size_t lonoLen) {
	char buf[32];
	int i = 0;
	char *lat = buf, *lon = 0;
	strncpy(buf, latlon, sizeof(buf) - 1);
	while (buf[i] != 0) {
		if (buf[i] == ',') {
			buf[i] = 0;
			if (sizeof(buf) - 1 - i >= 9) { // at least 9 chars of longitude
				lon = buf + i + 1;
			}
			break;
		}
		i++;
	}
	if (lat && lon) {
		strncpy(lato, lat, latoLen);
		strncpy(lono, lon, lonoLen);
		return true;
	}
	return false;
}


static void convert_to_dms(char* input, double* deg, double *min, double *sec) {
	double l1 = atof(input);
	double l1_deg = trunc(l1);
	double l1_min_full = (l1 - l1_deg) * 60.f;
	double l1_min = trunc(l1_min_full);
	double l1_sec = l1_min_full - l1_min;

	/*
	 printf("%f\n",l1);
	 printf("%f\n",l1_deg);
	 printf("%f\n",l1_min_full);
	 printf("%f\n",l1_min);
	 printf("%f\n",l1_sec);
	*/

	*deg = l1_deg;
	*min = l1_min;
	*sec = l1_sec;
}

/*
 * latlon splited by " 30.273815,120.144578" --> 30/16/25.734,120/08/
 * .lat = "3012.48N","12008.48E",
 */
void aprs_calc_location(char* latlon, char* out, size_t len) {
	char lat[10], lon[10];
	if (!parse_location_arg(latlon, lat, sizeof(lat) - 1, lon,
			sizeof(lon) - 1)) {
		return;
	}

	// calculate the input xx.xxxx to
	double lat_deg, lat_min, lat_sec;
	double lon_deg, lon_min, lon_sec;
	convert_to_dms(lat, &lat_deg, &lat_min, &lat_sec);
	convert_to_dms(lon, &lon_deg, &lon_min, &lon_sec);

	snprintf(out, len, "%02.0f%02.0f.%02.0f%c,%02.0f%02.0f.%02.0f%c", lat_deg,
			lat_min, (lat_sec * 100), (lat_deg > 0 ? 'N' : 'S'), lon_deg,
			lon_min, (lon_sec * 100), (lon_deg > 0 ? 'E' : 'W'));
}

#define kkey 0x73e2
short aprs_calc_hash(const char* thecall){
  char rootcall[10];     // need to copy call to remove ssid from parse
  char *p1 = rootcall;

  while ((*thecall != '-') && (*thecall != 0)) *p1++ = toupper(*thecall++);
    *p1 = 0;

  short hash = kkey;     // initialize with the key value
  short i = 0;
  short len = strlen(rootcall);
  char *ptr = rootcall;

  while (i < len) {         // loop through the string two bytes at a time
    hash ^= (*ptr++)<<8;   // xor high byte with accumulated hash
    hash ^= (*ptr++);     // xor low byte with accumulated hash
    i += 2;
  }
  return hash & 0x7fff;     // mask off the high bit so number is always positive
}

