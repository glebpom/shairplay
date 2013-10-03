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

#include <jack/jack.h>
#include <jack/ringbuffer.h>


#include "config.h"



#define max(x,y) (((x)>(y)) ? (x) : (y))
#define min(x,y) (((x)<(y)) ? (x) : (y))

 typedef struct {
	char apname[56];
	char password[56];
	unsigned short port;
	char hwaddr[6];

 } shairplay_options_t;

 typedef struct {
	 float volume;
	 jack_ringbuffer_t *rb;
 } shairplay_session_t;


 jack_port_t *jack_output_port1, *jack_output_port2;
 jack_client_t *jack_client;


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

shairplay_session_t *session;

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

#define SAMPLE_MAX_16BIT  32768.0f

 void sample_move_dS_s16_volume(jack_default_audio_sample_t* dst, char *src, jack_nframes_t nsamples, unsigned long src_skip, float volume) 
 {
	/* ALERT: signed sign-extension portability !!! */
	while (nsamples--) {
		*dst = (*((short *) src)) / SAMPLE_MAX_16BIT * volume;
		dst++;
		src += src_skip;
	}
 }	

 int
 process (jack_nframes_t nframes, void *arg)
 {   
	shairplay_session_t *session = (shairplay_session_t*)arg;

	jack_default_audio_sample_t *out1, *out2;

	out1 = (jack_default_audio_sample_t*)jack_port_get_buffer (jack_output_port1, nframes);
	out2 = (jack_default_audio_sample_t*)jack_port_get_buffer (jack_output_port2, nframes);

	jack_nframes_t nframes_left = nframes;
	int wrotebytes = 0;


	if (jack_ringbuffer_read_space(session->rb) < 100000) {

		// just write silence
		memset(out1, 0, nframes * sizeof(jack_default_audio_sample_t));
		memset(out2, 0, nframes * sizeof(jack_default_audio_sample_t));	
	} else {

		jack_ringbuffer_data_t rb_data[2];

		jack_ringbuffer_get_read_vector(session->rb, rb_data);

		while (nframes_left > 0 && rb_data[0].len > 4) {

			jack_nframes_t towrite_frames = (rb_data[0].len) / (sizeof(short) * 2);
			towrite_frames = min(towrite_frames, nframes_left);

			sample_move_dS_s16_volume(out1 + (nframes - nframes_left), (char *) rb_data[0].buf, towrite_frames, sizeof(short) * 2, session->volume);
			sample_move_dS_s16_volume(out2 + (nframes - nframes_left), (char *) rb_data[0].buf + sizeof(short), towrite_frames, sizeof(short) * 2, session->volume);


			wrotebytes = towrite_frames * sizeof(short) * 2;
			nframes_left -= towrite_frames;

			jack_ringbuffer_read_advance(session->rb, wrotebytes);
			jack_ringbuffer_get_read_vector(session->rb, rb_data);
		}

		if (nframes_left > 0) {
				// write silence
			memset(out1 + (nframes - nframes_left), 0, (nframes_left) * sizeof(jack_default_audio_sample_t));
			memset(out2 + (nframes - nframes_left), 0, (nframes_left) * sizeof(jack_default_audio_sample_t));		
		}

	}

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

static void * initialize_jack(char *client_name){
	shairplay_session_t *session;
//	shairplay_options_t *options = cls;

	const char **ports;
	const char *server_name = NULL;
	jack_options_t jack_options = JackNullOption;
	jack_status_t jack_status;

	session = calloc(1, sizeof(shairplay_session_t));
	assert(session);

	session->volume = 1.0f;

	session->rb = jack_ringbuffer_create(1048576);

	memset(session->rb->buf, 0, session->rb->size);


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

		 jack_set_process_callback (jack_client, process, session);

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
// #ifdef WIN32
// 	signal(SIGINT, signal_handler);
//     signal(SIGABRT, signal_handler);
// 	signal(SIGTERM, signal_handler);
// #else
// 	signal(SIGQUIT, signal_handler);
// 	signal(SIGTERM, signal_handler);
// 	signal(SIGHUP, signal_handler);
// 	signal(SIGINT, signal_handler);
// #endif
	//---jack---
	 fprintf(stderr, "Jack initialized\n");

	 return session;
}

static void destroy_jack(shairplay_session_t *session){
		fprintf(stderr, "Closing jack...");
		jack_client_close (jack_client);

		fprintf(stderr, "Freeing memory...");

		jack_ringbuffer_free (session->rb);

		free(session);

		fprintf(stderr, "Done...");
}


 static void *
 audio_init(void *cls, int bits, int channels, int samplerate)
 {
 		assert(bits == 16);
 		assert(channels == 2);
 		assert(samplerate == 44100);

 		return session;
	}


	static void
	audio_process(void *cls, void *opaque, char *buffer, int buflen)
	{
		shairplay_session_t *session = opaque;

		jack_ringbuffer_write (session->rb, buffer, buflen);
	}

	static void
	audio_destroy(void *cls, void *opaque)
	{
		sleep(20000);
		//?
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
			} else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
				fprintf(stderr, "Shairplay version %s\n", VERSION);
				fprintf(stderr, "Usage: %s [OPTION...]\n", path);
				fprintf(stderr, "\n");
				fprintf(stderr, "  -a, --apname=AirPort            Sets Airport name\n");
				fprintf(stderr, "  -p, --password=secret           Sets password\n");
				fprintf(stderr, "  -o, --server_port=5000          Sets port for RAOP service\n");
				fprintf(stderr, "      --hwaddr=address            Sets the MAC address, useful if running multiple instances\n");
				fprintf(stderr, "  -h, --help                      This help\n");
				fprintf(stderr, "\n");
				return 1;
			}
		}

		return 0;
	}


//--jack---


	int
	main(int argc, char *argv[])
	{
		shairplay_options_t options;

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

		session = initialize_jack(options.apname);

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

		destroy_jack(session);

		dnssd_unregister_raop(dnssd); 
		dnssd_destroy(dnssd);

		raop_stop(raop);
		raop_destroy(raop);


		exit (0);
	}
