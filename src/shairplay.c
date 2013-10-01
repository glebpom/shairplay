/**
 *  Copyright (C) 2012-2013  Juho Vähä-Herttua
 *
 *  Permission is hereby granted, free of charge, to any person obtaining
 *  a copy of this software and associated documentation files (the
 *  "Software"), to deal in the Software without restriction, including
 *  without limitation the rights to use, copy, modify, merge, publish,
 *  distribute, sublicense, and/or sell copies of the Software, and to
 *  permit persons to whom the Software is furnished to do so, subject to
 *  the following conditions:
 *  
 *  The above copyright notice and this permission notice shall be included
 *  in all copies or substantial portions of the Software.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>

#ifdef WIN32
# include <windows.h>
#endif

#include <shairplay/dnssd.h>
#include <shairplay/raop.h>

#include <ao/ao.h>
#include <jack/jack.h>

#include "config.h"

typedef struct {
	char apname[56];
	char password[56];
	unsigned short port;
	char hwaddr[6];

	char ao_driver[56];
	char ao_devicename[56];
	char ao_deviceid[16];
} shairplay_options_t;

typedef struct {
	ao_device *device;

	int buffering;
	int buflen;
	char buffer[8192];

	float volume;
} shairplay_session_t;


static int running;

#ifndef WIN32

#include <signal.h>
static void
signal_handler(int sig)
{
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		running = 0;
		break;
	}
}
static void
init_signals(void)
{
	struct sigaction sigact;

	sigact.sa_handler = signal_handler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
}

#endif


static int
parse_hwaddr(const char *str, char *hwaddr, int hwaddrlen)
{
	int slen, i;

	slen = 3*hwaddrlen-1;
	if (strlen(str) != slen) {
		return 1;
	}
	for (i=0; i<slen; i++) {
		if (str[i] == ':' && (i%3 == 2)) {
			continue;
		}
		if (str[i] >= '0' && str[i] <= '9') {
			continue;
		}
		if (str[i] >= 'a' && str[i] <= 'f') {
			continue;
		}
		return 1;
	}
	for (i=0; i<hwaddrlen; i++) {
		hwaddr[i] = (char) strtol(str+(i*3), NULL, 16);
	}
	return 0;
}

static ao_device *
audio_open_device(shairplay_options_t *opt, int bits, int channels, int samplerate)
{
	ao_device *device = NULL;
	ao_option *ao_options = NULL;
	ao_sample_format format;
	int driver_id;

	/* Get the libao driver ID */
	if (strlen(opt->ao_driver)) {
		driver_id = ao_driver_id(opt->ao_driver);
	} else {
		driver_id = ao_default_driver_id();
	}

	/* Add all available libao options */
	if (strlen(opt->ao_devicename)) {
		ao_append_option(&ao_options, "dev", opt->ao_devicename);
	}
	if (strlen(opt->ao_deviceid)) {
		ao_append_option(&ao_options, "id", opt->ao_deviceid);
	}

	/* Set audio format */
	memset(&format, 0, sizeof(format));
	format.bits = bits;
	format.channels = channels;
	format.rate = samplerate;
	format.byte_format = AO_FMT_NATIVE;

	/* Try opening the actual device */
	device = ao_open_live(driver_id, &format, ao_options);
	ao_free_options(ao_options);
	return device;
}

static void *
audio_init(void *cls, int bits, int channels, int samplerate)
{
	shairplay_options_t *options = cls;
	shairplay_session_t *session;

	session = calloc(1, sizeof(shairplay_session_t));
	assert(session);

	session->device = audio_open_device(options, bits, channels, samplerate);
	if (session->device == NULL) {
		printf("Error opening device %d\n", errno);
	}
	assert(session->device);

	session->buffering = 1;
	session->volume = 1.0f;
	return session;
}

static int
audio_output(shairplay_session_t *session, const void *buffer, int buflen)
{
	short *shortbuf;
	char tmpbuf[4096];
	int tmpbuflen, i;

	tmpbuflen = (buflen > sizeof(tmpbuf)) ? sizeof(tmpbuf) : buflen;
	memcpy(tmpbuf, buffer, tmpbuflen);
	if (ao_is_big_endian()) {
		for (i=0; i<tmpbuflen/2; i++) {
			char tmpch = tmpbuf[i*2];
			tmpbuf[i*2] = tmpbuf[i*2+1];
			tmpbuf[i*2+1] = tmpch;
		}
	}
	shortbuf = (short *)tmpbuf;
	for (i=0; i<tmpbuflen/2; i++) {
		shortbuf[i] = shortbuf[i] * session->volume;
	}
	ao_play(session->device, tmpbuf, tmpbuflen);
	return tmpbuflen;
}

static void
audio_process(void *cls, void *opaque, const void *buffer, int buflen)
{
	shairplay_session_t *session = opaque;
	int processed;

	if (session->buffering) {
		printf("Buffering...\n");
		if (session->buflen+buflen < sizeof(session->buffer)) {
			memcpy(session->buffer+session->buflen, buffer, buflen);
			session->buflen += buflen;
			return;
		}
		session->buffering = 0;
		printf("Finished buffering...\n");

		processed = 0;
		while (processed < session->buflen) {
			processed += audio_output(session,
			                          session->buffer+processed,
			                          session->buflen-processed);
		}
		session->buflen = 0;
	}

	processed = 0;
	while (processed < buflen) {
		processed += audio_output(session,
		                          buffer+processed,
		                          buflen-processed);
	}
}

static void
audio_destroy(void *cls, void *opaque)
{
	shairplay_session_t *session = opaque;

	ao_close(session->device);
	free(session);
}

static void
audio_set_volume(void *cls, void *opaque, float volume)
{
	shairplay_session_t *session = opaque;
	session->volume = pow(10.0, 0.05*volume);
}

static int
parse_options(shairplay_options_t *opt, int argc, char *argv[])
{
	const char default_hwaddr[] = { 0x48, 0x5d, 0x60, 0x7c, 0xee, 0x22 };

	char *path = argv[0];
	char *arg;

	/* Set default values for apname and port */
	strncpy(opt->apname, "Shairplay", sizeof(opt->apname)-1);
	opt->port = 5000;
	memcpy(opt->hwaddr, default_hwaddr, sizeof(opt->hwaddr));

	while ((arg = *++argv)) {
		if (!strcmp(arg, "-a")) {
			strncpy(opt->apname, *++argv, sizeof(opt->apname)-1);
		} else if (!strncmp(arg, "--apname=", 9)) {
			strncpy(opt->apname, arg+9, sizeof(opt->apname)-1);
		} else if (!strcmp(arg, "-p")) {
			strncpy(opt->password, *++argv, sizeof(opt->password)-1);
		} else if (!strncmp(arg, "--password=", 11)) {
			strncpy(opt->password, arg+11, sizeof(opt->password)-1);
		} else if (!strcmp(arg, "-o")) {
			opt->port = atoi(*++argv);
		} else if (!strncmp(arg, "--server_port=", 14)) {
			opt->port = atoi(arg+14);
		} else if (!strncmp(arg, "--hwaddr=", 9)) {
			if (parse_hwaddr(arg+9, opt->hwaddr, sizeof(opt->hwaddr))) {
				fprintf(stderr, "Invalid format given for hwaddr, aborting...\n");
				fprintf(stderr, "Please use hwaddr format: 01:45:89:ab:cd:ef\n");
				return 1;
			}
		} else if (!strncmp(arg, "--ao_driver=", 12)) {
			strncpy(opt->ao_driver, arg+12, sizeof(opt->ao_driver)-1);
		} else if (!strncmp(arg, "--ao_devicename=", 16)) {
			strncpy(opt->ao_devicename, arg+16, sizeof(opt->ao_devicename)-1);
		} else if (!strncmp(arg, "--ao_deviceid=", 14)) {
			strncpy(opt->ao_deviceid, arg+14, sizeof(opt->ao_deviceid)-1);
		} else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
			fprintf(stderr, "Shairplay version %s\n", VERSION);
			fprintf(stderr, "Usage: %s [OPTION...]\n", path);
			fprintf(stderr, "\n");
			fprintf(stderr, "  -a, --apname=AirPort            Sets Airport name\n");
			fprintf(stderr, "  -p, --password=secret           Sets password\n");
			fprintf(stderr, "  -o, --server_port=5000          Sets port for RAOP service\n");
			fprintf(stderr, "      --hwaddr=address            Sets the MAC address, useful if running multiple instances\n");
			fprintf(stderr, "      --ao_driver=driver          Sets the ao driver (optional)\n");
			fprintf(stderr, "      --ao_devicename=devicename  Sets the ao device name (optional)\n");
			fprintf(stderr, "      --ao_deviceid=id            Sets the ao device id (optional)\n");
			fprintf(stderr, "  -h, --help                      This help\n");
			fprintf(stderr, "\n");
			return 1;
		}
	}

	return 0;
}


//---jack---

jack_port_t *jack_output_port1, *jack_output_port2;
jack_client_t *jack_client;

/**
 * The process callback for this JACK application is called in a
 * special realtime thread once for each audio cycle.
 *
 * This client follows a simple rule: when the JACK transport is
 * running, copy the input port to the output.  When it stops, exit.
 */

int
process (jack_nframes_t nframes, void *arg)
{   
	return 0;      
}

/**
 * JACK calls this shutdown_callback if the server ever shuts down or
 * decides to disconnect the client.
 */
void
jack_shutdown (void *arg)
{
	exit (1);
}

//--jack---


int
main(int argc, char *argv[])
{
	shairplay_options_t options;
	ao_device *device = NULL;

	dnssd_t *dnssd;
	raop_t *raop;
	raop_callbacks_t raop_cbs;
	char *password = NULL;

	int error;

#ifndef WIN32
	init_signals();
#endif

	memset(&options, 0, sizeof(options));
	if (parse_options(&options, argc, argv)) {
		return 0;
	}

	//---jack---

	const char **ports;
	const char *client_name = "shairplay";
	const char *server_name = NULL;
	jack_options_t jack_options = JackNullOption;
	jack_status_t jack_status;


	jack_client = jack_client_open (client_name, jack_options, &jack_status, server_name);
	if (jack_client == NULL) {
		fprintf (stderr, "jack_client_open() failed, "
			 "status = 0x%2.0x\n", jack_status);
		if (jack_status & JackServerFailed) {
			fprintf (stderr, "Unable to connect to JACK server\n");
		}
		exit (1);
	}
	if (jack_status & JackServerStarted) {
		fprintf (stderr, "JACK server started\n");
	}
	if (jack_status & JackNameNotUnique) {
		client_name = jack_get_client_name(jack_client);
		fprintf (stderr, "unique name `%s' assigned\n", client_name);
	}

	/* tell the JACK server to call `process()' whenever
	   there is work to be done.
	*/

	jack_set_process_callback (jack_client, process, 0);

	/* tell the JACK server to call `jack_shutdown()' if
	   it ever shuts down, either entirely, or if it
	   just decides to stop calling us.
	*/

	jack_on_shutdown (jack_client, jack_shutdown, 0);

	/* create two ports */

	jack_output_port1 = jack_port_register (jack_client, "output1",
					  JACK_DEFAULT_AUDIO_TYPE,
					  JackPortIsOutput, 0);

	jack_output_port2 = jack_port_register (jack_client, "output2",
					  JACK_DEFAULT_AUDIO_TYPE,
					  JackPortIsOutput, 0);

	if ((jack_output_port1 == NULL) || (jack_output_port2 == NULL)) {
		fprintf(stderr, "no more JACK ports available\n");
		exit (1);
	}

	/* Tell the JACK server that we are ready to roll.  Our
	 * process() callback will start running now. */

	if (jack_activate (jack_client)) {
		fprintf (stderr, "cannot activate client");
		exit (1);
	}

	/* Connect the ports.  You can't do this before the client is
	 * activated, because we can't make connections to clients
	 * that aren't running.  Note the confusing (but necessary)
	 * orientation of the driver backend ports: playback ports are
	 * "input" to the backend, and capture ports are "output" from
	 * it.
	 */
 	
	ports = jack_get_ports (jack_client, NULL, NULL,
				JackPortIsPhysical|JackPortIsInput);
	if (ports == NULL) {
		fprintf(stderr, "no physical playback ports\n");
		exit (1);
	}

	if (jack_connect (jack_client, jack_port_name (jack_output_port1), ports[0])) {
		fprintf (stderr, "cannot connect output ports\n");
	}

	if (jack_connect (jack_client, jack_port_name (jack_output_port2), ports[1])) {
		fprintf (stderr, "cannot connect output ports\n");
	}

	jack_free (ports);
    
    /* install a signal handler to properly quits jack client */
#ifdef WIN32
	signal(SIGINT, signal_handler);
    signal(SIGABRT, signal_handler);
	signal(SIGTERM, signal_handler);
#else
	signal(SIGQUIT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGHUP, signal_handler);
	signal(SIGINT, signal_handler);
#endif
	//---jack---
	fprintf(stderr, "Jack initialized\n");


	ao_initialize();

	device = audio_open_device(&options, 16, 2, 44100);
	if (device == NULL) {
		fprintf(stderr, "Error opening audio device %d\n", errno);
		fprintf(stderr, "Please check your libao settings and try again\n");
		return -1;
	} else {
		ao_close(device);
		device = NULL;
	}

	memset(&raop_cbs, 0, sizeof(raop_cbs));
	raop_cbs.cls = &options;
	raop_cbs.audio_init = audio_init;
	raop_cbs.audio_process = audio_process;
	raop_cbs.audio_destroy = audio_destroy;
	raop_cbs.audio_set_volume = audio_set_volume;

	raop = raop_init_from_keyfile(10, &raop_cbs, "airport.key", NULL);
	if (raop == NULL) {
		fprintf(stderr, "Could not initialize the RAOP service\n");
		return -1;
	}

	if (strlen(options.password)) {
		password = options.password;
	}
	raop_set_log_level(raop, RAOP_LOG_DEBUG);
	raop_start(raop, &options.port, options.hwaddr, sizeof(options.hwaddr), password);

	error = 0;
	dnssd = dnssd_init(&error);
	if (error) {
		fprintf(stderr, "ERROR: Could not initialize dnssd library!\n");
		fprintf(stderr, "------------------------------------------\n");
		fprintf(stderr, "You could try the following resolutions based on your OS:\n");
		fprintf(stderr, "Windows: Try installing http://support.apple.com/kb/DL999\n");
		fprintf(stderr, "Debian/Ubuntu: Try installing libavahi-compat-libdnssd-dev package\n");
		raop_destroy(raop);
		return -1;
	}

	dnssd_register_raop(dnssd, options.apname, options.port, options.hwaddr, sizeof(options.hwaddr), 0);

	running = 1;
	while (running) {
#ifndef WIN32
		sleep(1);
#else
		Sleep(1000);
#endif
	}

	dnssd_unregister_raop(dnssd); 
	dnssd_destroy(dnssd);

	raop_stop(raop);
	raop_destroy(raop);

	ao_shutdown();

	jack_client_close (jack_client);
	exit (0);
}
