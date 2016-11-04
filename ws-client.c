#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <errno.h>

#include <unistd.h>

#include <pulse/cdecl.h>
#include <pulse/simple.h>
#include <pulse/error.h>


#include "libwebsockets.h"

static const char* URL = "/speech-to-text/api/v1/recognize";

static const char* URL2 = "?watson-token=";

static const char* json = "{ \"username\": \"bc2a819e-ffad-4543-8db9-b44bf3f968d3\", \"password\": \"tqlnmhZerLQD\", \"version\": \"v1\"}";

static const char* model = "&model=en-US_BroadbandModel";

static const char* startmsg = "{ \"action\": \"start\", \"content-type\": \"audio/l16;rate=44100;channels=2\", \
\"interim_results\": true, \"continuous\": true, \"word_confidence\": true, \"timestamps\": true, \"max_alternatives\": 3, \
\"inactivity_timeout\": 600, \"word_alternatives_threshold\": 0.001, \"smart_formatting\": true }";

// \"keywords_threshold\": keywords_threshold, \"keywords\": keywords,

static unsigned int opts;
static int test_post;
static int was_closed;
static int deny_deflate;
static int deny_mux;
static volatile int force_exit = 0;
static int longlived = 0;
static bool established = false;
#if defined(LWS_USE_POLARSSL)
#else
#if defined(LWS_USE_MBEDTLS)
#else
#if defined(LWS_OPENSSL_SUPPORT) && defined(LWS_HAVE_SSL_CTX_set1_param)
char crl_path[1024] = "";
#endif
#endif
#endif
static struct lws *wsi_client;
static struct lws_context *context;

pthread_mutex_t lock;
uint8_t* audioBuffer;
uint32_t bytes;
bool newData = false;
bool dataRead = false;
bool sendStart = true;
bool listening = false;

pa_simple* pr;
pa_simple* pw;
pa_sample_spec ss = { PA_SAMPLE_S16LE, 44100, 2 };
const char* device = NULL; //"alsa_input.usb-046d_HD_Pro_Webcam_C920_7F4EBD4F-02-C920.analog-stereo";

pthread_t pa_thread;


static uint8_t* sendBuffer = NULL;
static uint32_t sendBufferSize = 0;
static int SendData(struct lws *wsi, const uint8_t* data, uint32_t dataSize, bool text)
{
	size_t sendSize = LWS_SEND_BUFFER_PRE_PADDING
			+ dataSize +
			LWS_SEND_BUFFER_POST_PADDING;

	if (!sendBuffer || (sendBufferSize < sendSize)) {
		if (sendBuffer) {
			free(sendBuffer);
		}
		sendBuffer = (uint8_t*) malloc(sendSize);
		sendBufferSize = sendSize;
	}

	memcpy(&sendBuffer[LWS_SEND_BUFFER_PRE_PADDING], data, dataSize);

	int count = 2;
	while (count-- > 0)
	{
		size_t m = lws_write(wsi,
				&sendBuffer[LWS_SEND_BUFFER_PRE_PADDING],
				dataSize, (text ? LWS_WRITE_TEXT : LWS_WRITE_BINARY));

		if (m >= dataSize)
		{
			break;
		}
		else if (count == 0)
		{
			fprintf(stderr, "WebSocketConsole: Error sending data: %zu %u", m, dataSize);
		}

	}

	return 0;
}

static int
callback_client(struct lws *wsi,
			enum lws_callback_reasons reason,
					       void *user, void *in, size_t len)
{
	switch (reason) {

	case LWS_CALLBACK_CLIENT_ESTABLISHED:
		// fprintf(stderr, "callback_client: LWS_CALLBACK_CLIENT_ESTABLISHED\n");

		_lws_log(LLL_NOTICE, "LWS_CALLBACK_CLIENT_ESTABLISHED\n");
    established = true;
		sendStart = true;

		lws_callback_on_writable(wsi);

		break;

	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		_lws_log(LLL_NOTICE, "LWS_CALLBACK_CLIENT_CONNECTION_ERROR\n");
		if (in) {
			_lws_log(LLL_NOTICE, "%s\n", in);
		}
		was_closed = 1;
		break;

	case LWS_CALLBACK_CLOSED:
		// fprintf(stderr, "LWS_CALLBACK_CLOSED\n");
		was_closed = 1;
		listening = false;
		established = false;
		_lws_log(LLL_NOTICE, "LWS_CALLBACK_CLOSED\n");
		break;

	case LWS_CALLBACK_CLIENT_RECEIVE:
		((char *)in)[len] = '\0';
		if (strstr(in, "listening") != NULL) {
			listening = true;
			lws_callback_on_writable(wsi);

			_lws_log(LLL_NOTICE, "Listening\n");
		}
		else {
			_lws_log(LLL_NOTICE, "rx %d '%s'\n", (int)len, (char *)in);
		}

		// fprintf(stderr, "rx %d '%s'\n", (int)len, (char *)in);
		break;

	case LWS_CALLBACK_CLIENT_WRITEABLE:
	{
		_lws_log(LLL_NOTICE, "LWS_CALLBACK_CLIENT_WRITEABLE\n");
		if (sendStart) {
			SendData(wsi, startmsg, strlen(startmsg), true);
			sendStart = false;
		}

		if (listening) {
			pthread_mutex_lock(&lock);
			if (newData) {
				SendData(wsi, audioBuffer, bytes, false);
			}
			else {
				fprintf(stderr, "No data\n");
			}
			newData = false;
			dataRead = true;
			pthread_mutex_unlock(&lock);
		}
	}
	break;

	/* because we are protocols[0] ... */

	case LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED:
		if ((strcmp(in, "deflate-stream") == 0) && deny_deflate) {
			fprintf(stderr, "denied deflate-stream extension\n");
			return 1;
		}
		if ((strcmp(in, "deflate-frame") == 0) && deny_deflate) {
			fprintf(stderr, "denied deflate-frame extension\n");
			return 1;
		}
		if ((strcmp(in, "x-google-mux") == 0) && deny_mux) {
			fprintf(stderr, "denied x-google-mux extension\n");
			return 1;
		}

		break;

	default:
		break;
	}

	return 0;
}

/* list of supported protocols and callbacks */
static struct lws_protocols protocols[] = {
	{
		"",
		callback_client,
		0,
		16384,
	},
	{ NULL, NULL, 0, 0 } /* end */
};

static void* paThread(void* arg) {
	pa_buffer_attr attr;
	attr.maxlength = -1;
	attr.tlength   = -1;
	attr.prebuf    = -1;
	attr.minreq    = -1;
	attr.fragsize  = 4*512;

	int error;
	pr = pa_simple_new(NULL, "NzLinuxXMic", PA_STREAM_RECORD, device,
			"record", &ss, NULL, &attr, &error);
	if (!pr)
	{
		fprintf(stderr, "Unable to open pulse audio for source %s\n", pa_strerror(error));
		exit -1;
	}

	pw = pa_simple_new(NULL, "monitor", PA_STREAM_PLAYBACK, NULL,
			"playback", &ss, NULL, NULL, &error);

	if (!pw)
	{
		lwsl_err("Unable to open pulse audio for sink %s\n", pa_strerror(error));
	}

	fprintf(stderr, "Opened microphone for reading\n");
	uint32_t sampleSize = 4 * 256;
	audioBuffer = (uint8_t*)malloc(sampleSize);
	const size_t sampleSizeDword = sampleSize / sizeof(uint32_t);

	while (true) {
		struct timespec t;

		while (established) {
			if (pa_simple_read(pr, audioBuffer, sampleSize, &error) < 0) {
				lwsl_err("pa_simple_read() failed: %s", pa_strerror(error));
				exit -1;
			}

			if (pw)
				if (pa_simple_write(pw, audioBuffer, sampleSize, &error) < 0) {
					lwsl_notice("pa_simple_write() failed: %s", pa_strerror(error));
				}

			// Pulse audio returns an empty audio packet even if there's no audio
      // playing, so we need to detect if this is empty.
      // Note: Sample size MUST be a multiple of of 4.
      uint32_t * pData = (uint32_t *) audioBuffer;
      bool hasAudio = false;
			size_t index;
      for(index = 0; index < sampleSizeDword && !hasAudio; index++, pData++)
      {
      	if (*pData)
        	hasAudio = true;
      }

      // Play the audio.
      if (true) {
				pthread_mutex_lock(&lock);
				bytes = sampleSize;
				dataRead = false;
				newData = true;
				pthread_mutex_unlock(&lock);

				lwsl_notice("Has audio\n");
				lws_callback_on_writable(wsi_client);
				
				bool done = false;
				while (!done && !was_closed) {
					pthread_mutex_lock(&lock);
					done = dataRead;

					t.tv_sec = 0;
					t.tv_nsec = 1000000;
					nanosleep(&t, NULL);
					pthread_mutex_unlock(&lock);
				}
			}
		}
		pthread_mutex_lock(&lock);
		dataRead = false;
		newData = true;
		pthread_mutex_unlock(&lock);
		t.tv_sec = 0;
		t.tv_nsec = 1000000;
		nanosleep(&t, NULL);
	}
}


void sighandler(int sig)
{
	force_exit = 1;
}

static struct option options[] = {
	{ "help",	no_argument,		NULL, 'h' },
	{ "debug",      required_argument,      NULL, 'd' },
	{ "port",	required_argument,	NULL, 'p' },
	{ "ssl",	no_argument,		NULL, 's' },
	{ "strict-ssl",	no_argument,		NULL, 'S' },
	{ "version",	required_argument,	NULL, 'v' },
	{ "token", required_argument, NULL, 't'},
	{ "undeflated",	no_argument,		NULL, 'u' },
	{ "longlived",	no_argument,		NULL, 'l' },
	{ "post",	no_argument,		NULL, 'o' },
	{ "pingpong-secs", required_argument,	NULL, 'P' },
	{ "ssl-cert",  required_argument,	NULL, 'C' },
	{ "ssl-key",  required_argument,	NULL, 'K' },
	{ "ssl-ca",  required_argument,		NULL, 'A' },
#if defined(LWS_OPENSSL_SUPPORT) && defined(LWS_HAVE_SSL_CTX_set1_param)
	{ "ssl-crl",  required_argument,		NULL, 'R' },
#endif
	{ NULL, 0, 0, 0 }
};

static int ratelimit_connects(unsigned int *last, unsigned int secs)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	if (tv.tv_sec - (*last) < secs)
		return 0;

	*last = tv.tv_sec;

	return 1;
}

int main(int argc, char **argv)
{
	int n = 0, m, ret = 0, port = 7681, use_ssl = 0, ietf_version = -1;
	unsigned int rl_client = 0, do_ws = 1, pp_secs = 0;
	struct lws_context_creation_info info;
	struct lws_client_connect_info i;
	const char *prot, *p;
	char path[2048];
	char cert_path[1024] = "";
	char key_path[1024] = "";
	char ca_path[1024] = "";
	char token[1024] = {0};

	memset(&info, 0, sizeof info);

	if (argc < 2)
		goto usage;

	while (n >= 0) {
		n = getopt_long(argc, argv, "Snuv:hsp:d:lC:K:A:P:t:mo", options, NULL);
		if (n < 0)
			continue;
		switch (n) {
		case 'd':
			lws_set_log_level(atoi(optarg), NULL);
			break;
		case 's': /* lax SSL, allow selfsigned, skip checking hostname */
			use_ssl = LCCSCF_USE_SSL |
				  LCCSCF_ALLOW_SELFSIGNED |
				  LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
			break;
		case 'S': /* Strict SSL, no selfsigned, check server hostname */
			use_ssl = LCCSCF_USE_SSL;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'P':
			pp_secs = atoi(optarg);
			lwsl_notice("Setting pingpong interval to %d\n", pp_secs);
			break;
		case 'l':
			longlived = 1;
			break;
		case 'v':
			ietf_version = atoi(optarg);
			break;
		case 'u':
			deny_deflate = 1;
			break;
		case 'o':
			test_post = 1;
			break;
		case 't':
			strcpy(token, optarg);
			lwsl_notice("Token size: %d\n", strlen(token));
			break;
		case 'C':
			strncpy(cert_path, optarg, sizeof(cert_path) - 1);
			cert_path[sizeof(cert_path) - 1] = '\0';
			break;
		case 'K':
			strncpy(key_path, optarg, sizeof(key_path) - 1);
			key_path[sizeof(key_path) - 1] = '\0';
			break;
		case 'A':
			strncpy(ca_path, optarg, sizeof(ca_path) - 1);
			ca_path[sizeof(ca_path) - 1] = '\0';
			break;
#if defined(LWS_USE_POLARSSL)
#else
#if defined(LWS_USE_MBEDTLS)
#else
#if defined(LWS_OPENSSL_SUPPORT) && defined(LWS_HAVE_SSL_CTX_set1_param)
		case 'R':
			strncpy(crl_path, optarg, sizeof(crl_path) - 1);
			crl_path[sizeof(crl_path) - 1] = '\0';
			break;
#endif
#endif
#endif
		case 'h':
			goto usage;
		}
	}

	if (optind >= argc)
		goto usage;

	signal(SIGINT, sighandler);

	memset(&i, 0, sizeof(i));

	i.port = port;
	if (lws_parse_uri(argv[optind], &prot, &i.address, &i.port, &p))
		goto usage;

	/* add back the leading / on path */
	path[0] = '/';
	strcpy(&path[1], p);
	strcpy(&path[strlen(path)], URL2);
	strcpy(&path[strlen(path)], token);
	strcpy(&path[strlen(path)], model);

	i.path = path;

	fprintf(stderr, "Path: -%s-\n", path);

	if (!strcmp(prot, "http") || !strcmp(prot, "ws"))
		use_ssl = 0;
	if (!strcmp(prot, "https") || !strcmp(prot, "wss"))
		if (!use_ssl)
			use_ssl = LCCSCF_USE_SSL;

	/*
	 * create the websockets context.  This tracks open connections and
	 * knows how to route any traffic and which protocol version to use,
	 * and if each connection is client or server side.
	 *
	 * For this client-only demo, we tell it to not listen on any port.
	 */

	info.port = CONTEXT_PORT_NO_LISTEN;
	info.protocols = protocols;
	info.gid = -1;
	info.uid = -1;
	info.ws_ping_pong_interval = pp_secs;
  // info.ssl_cipher_list = "aNULL";

	if (use_ssl) {
		info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

		/*
		 * If the server wants us to present a valid SSL client certificate
		 * then we can set it up here.
		 */

		if (cert_path[0])
			info.ssl_cert_filepath = cert_path;
		if (key_path[0])
			info.ssl_private_key_filepath = key_path;

		/*
		 * A CA cert and CRL can be used to validate the cert send by the server
		 */
		if (ca_path[0])
			info.ssl_ca_filepath = ca_path;
#if defined(LWS_USE_POLARSSL)
#else
#if defined(LWS_USE_MBEDTLS)
#else
#if defined(LWS_OPENSSL_SUPPORT) && defined(LWS_HAVE_SSL_CTX_set1_param)
		else if (crl_path[0])
			lwsl_notice("WARNING, providing a CRL requires a CA cert!\n");
#endif
#endif
#endif
	}

	if (use_ssl & LCCSCF_USE_SSL)
		lwsl_notice(" Using SSL\n");
	else
		lwsl_notice(" SSL disabled\n");
	if (use_ssl & LCCSCF_ALLOW_SELFSIGNED)
		lwsl_notice(" Selfsigned certs allowed\n");
	else
		lwsl_notice(" Cert must validate correctly (use -s to allow selfsigned)\n");
	if (use_ssl & LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK)
		lwsl_notice(" Skipping peer cert hostname check\n");
	else
		lwsl_notice(" Requiring peer cert hostname matches\n");

	context = lws_create_context(&info);
	if (context == NULL) {
		fprintf(stderr, "Creating libwebsocket context failed\n");
		return 1;
	}

	i.context = context;
	i.ssl_connection = use_ssl;
	i.host = i.address;
	i.origin = i.address;
	i.ietf_version_or_minus_one = ietf_version;
	// i.client_exts = exts;

	if (!strcmp(prot, "http") || !strcmp(prot, "https")) {
		lwsl_notice("using %s mode (non-ws)\n", prot);
		if (test_post) {
			i.method = "POST";
			lwsl_notice("POST mode\n");
		}
		else
			i.method = "GET";
		do_ws = 0;
	} else
		lwsl_notice("using %s mode (ws)\n", prot);

	// ---------------------
	if (pthread_mutex_init(&lock, NULL) != 0)
	{
			fprintf(stderr, "mutex init failed\n");
			exit -1;
	}

	if (pthread_create(&pa_thread, NULL, &paThread, (void*) NULL) != 0) {
		fprintf(stderr, "Failed to create the thread for microphone %s\n", strerror(errno));
	}
	// ---------------------

	m = 0;
	while (!force_exit) {

		if (do_ws) {
			if (!wsi_client && ratelimit_connects(&rl_client, 2u)) {
				lwsl_notice("client: connecting\n");
				i.protocol = protocols[0].name;
				i.pwsi = &wsi_client;
				lws_client_connect_via_info(&i);
			}

		} else {
			if (!wsi_client && ratelimit_connects(&rl_client, 2u)) {
				lwsl_notice("http: connecting\n");
				i.pwsi = &wsi_client;
				lws_client_connect_via_info(&i);
			}
		}

		lws_service(context, 10);

	}

	lwsl_err("Exiting\n");
	lws_context_destroy(context);

	return ret;

usage:
	fprintf(stderr, "Usage: libwebsockets-test-client "
				"<server address> [--port=<p>] "
				"[--ssl] [-k] [-v <ver>] "
				"[-d <log bitfield>] [-l]\n");
	return 1;
}
