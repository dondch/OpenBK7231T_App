// BK7252 camera driver: brings up the sensor pipeline and serves MJPEG over HTTP.
//
// The SDK already has everything below this file: camera_intf drives the sensor
// over SCCB, the hardware JPEG encoder produces frames, and video_transfer plus
// app/video_work/video_buffer.c hand them out one at a time. None of that is
// wired to anything by default, and video_buffer.c is commented out in
// beken378/beken_src.mk, so it has to be enabled there as well.
//
// The stream listens on its own port, NOT 80. OpenBeken's own web server owns
// port 80, and a multipart response never ends, so sharing the port would make
// the config UI unreachable for as long as anyone is watching. For the same
// reason this runs on its own thread rather than inside an HTTP handler.
//
// video_buffer.c ships without a header, so its three entry points are declared
// here. Plain C types are used rather than the SDK's UINT8/UINT32 to avoid
// pulling the Beken typedefs into the app layer; they are the same width.

#include "../new_common.h"
#include "../cmnds/cmd_public.h"
#include "../cmnds/cmd_local.h"
#include "../logging/logging.h"

#if ENABLE_DRIVER_BKCAMERA

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "../httpserver/new_http.h"

extern int video_buffer_open(void);
extern int video_buffer_close(void);
extern unsigned int video_buffer_read_frame(unsigned char *buf, unsigned int buf_len,
                                            int *err_code, unsigned int timeout);

#define CAM_BOUNDARY      "obkcamboundary"
#define CAM_DEFAULT_PORT  8080
// VGA frames from the GC0328 measure about 24 kB. 48 kB leaves room for a busy
// scene without letting one frame eat the heap.
#define CAM_FRAME_MAX     (48 * 1024)
#define CAM_READ_TIMEOUT  2000

static beken_thread_t g_camThread = NULL;
static int g_camPort = CAM_DEFAULT_PORT;
static volatile int g_camStop = 0;
static volatile int g_camRunning = 0;
static volatile int g_camFrames = 0;
static char g_camStatus[96] = "not started";

static int Cam_SendAll(int fd, const char *data, int len) {
	int sent = 0;
	while (sent < len) {
		int r = send(fd, data + sent, len - sent, 0);
		if (r <= 0)
			return -1;
		sent += r;
	}
	return 0;
}

static int Cam_ServeClient(int client, unsigned char *buf) {
	char hdr[192];
	int len;

	len = snprintf(hdr, sizeof(hdr),
		"HTTP/1.0 200 OK\r\n"
		"Connection: close\r\n"
		"Cache-Control: no-store, no-cache, must-revalidate\r\n"
		"Pragma: no-cache\r\n"
		"Content-Type: multipart/x-mixed-replace;boundary=" CAM_BOUNDARY "\r\n"
		"\r\n"
		"--" CAM_BOUNDARY "\r\n");
	if (Cam_SendAll(client, hdr, len) < 0)
		return -1;

	while (!g_camStop) {
		int err = 0;
		unsigned int frame = video_buffer_read_frame(buf, CAM_FRAME_MAX, &err, CAM_READ_TIMEOUT);
		if (frame == 0)
			continue;

		len = snprintf(hdr, sizeof(hdr),
			"Content-Type: image/jpeg\r\n"
			"Content-Length: %u\r\n"
			"\r\n", frame);
		if (Cam_SendAll(client, hdr, len) < 0)
			return -1;
		if (Cam_SendAll(client, (const char *)buf, (int)frame) < 0)
			return -1;
		len = snprintf(hdr, sizeof(hdr), "\r\n--" CAM_BOUNDARY "\r\n");
		if (Cam_SendAll(client, hdr, len) < 0)
			return -1;

		g_camFrames++;
	}
	return 0;
}

static void Cam_ServerThread(void *arg) {
	struct sockaddr_in addr;
	socklen_t slen = sizeof(addr);
	unsigned char *buf;
	int srv = -1;
	int on = 1;

	buf = (unsigned char *)malloc(CAM_FRAME_MAX);
	if (buf == NULL) {
		snprintf(g_camStatus, sizeof(g_camStatus), "malloc(%i) failed", CAM_FRAME_MAX);
		ADDLOG_ERROR(LOG_FEATURE_DRV, "BKCamera: no memory for a frame buffer");
		goto done;
	}

	// video_buffer_open() returns 1 on a fresh open and 0 if it was already
	// open; only negative values are errors (kGeneralErr -1, kOpenErr -6755)
	if (video_buffer_open() < 0) {
		snprintf(g_camStatus, sizeof(g_camStatus), "video_buffer_open failed");
		ADDLOG_ERROR(LOG_FEATURE_DRV, "BKCamera: video_buffer_open failed");
		goto done;
	}

	srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) {
		snprintf(g_camStatus, sizeof(g_camStatus), "socket() failed");
		ADDLOG_ERROR(LOG_FEATURE_DRV, "BKCamera: socket failed");
		goto done;
	}
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(g_camPort);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		snprintf(g_camStatus, sizeof(g_camStatus), "bind(%i) failed", g_camPort);
		ADDLOG_ERROR(LOG_FEATURE_DRV, "BKCamera: bind to port %i failed", g_camPort);
		goto done;
	}
	if (listen(srv, 1) != 0) {
		snprintf(g_camStatus, sizeof(g_camStatus), "listen() failed");
		ADDLOG_ERROR(LOG_FEATURE_DRV, "BKCamera: listen failed");
		goto done;
	}

	g_camRunning = 1;
	snprintf(g_camStatus, sizeof(g_camStatus), "listening on %i", g_camPort);
	ADDLOG_INFO(LOG_FEATURE_DRV, "BKCamera: MJPEG stream on port %i", g_camPort);

	while (!g_camStop) {
		int client = accept(srv, (struct sockaddr *)&addr, &slen);
		if (client < 0)
			continue;
		ADDLOG_INFO(LOG_FEATURE_DRV, "BKCamera: client connected");
		Cam_ServeClient(client, buf);
		close(client);
		ADDLOG_INFO(LOG_FEATURE_DRV, "BKCamera: client gone after %i frames", g_camFrames);
	}

done:
	if (srv >= 0)
		close(srv);
	if (buf != NULL)
		free(buf);
	video_buffer_close();
	g_camRunning = 0;
	g_camThread = NULL;
	strcat(g_camStatus, " (thread exited)");
	rtos_delete_thread(NULL);
}

static commandResult_t CMD_CamStart(const void *context, const char *cmd, const char *args, int cmdFlags) {
	if (g_camRunning || g_camThread != NULL) {
		ADDLOG_INFO(LOG_FEATURE_DRV, "BKCamera: already running on port %i", g_camPort);
		return CMD_RES_OK;
	}
	Tokenizer_TokenizeString(args, 0);
	g_camPort = Tokenizer_GetArgIntegerDefault(0, CAM_DEFAULT_PORT);
	g_camStop = 0;
	g_camFrames = 0;

	if (rtos_create_thread(&g_camThread, BEKEN_APPLICATION_PRIORITY, "obkcam",
			(beken_thread_function_t)Cam_ServerThread, 2048, 0) != kNoErr) {
		snprintf(g_camStatus, sizeof(g_camStatus), "rtos_create_thread failed");
		ADDLOG_ERROR(LOG_FEATURE_DRV, "BKCamera: could not start the server thread");
		g_camThread = NULL;
		return CMD_RES_ERROR;
	}
	return CMD_RES_OK;
}

static commandResult_t CMD_CamStop(const void *context, const char *cmd, const char *args, int cmdFlags) {
	g_camStop = 1;
	ADDLOG_INFO(LOG_FEATURE_DRV, "BKCamera: stopping");
	return CMD_RES_OK;
}

void BKCamera_Init(void) {
	//cmddetail:{"name":"CAM_Start","args":"[Port]",
	//cmddetail:"descr":"Starts the MJPEG stream. Default port 8080, because 80 is the config UI.",
	//cmddetail:"fn":"CMD_CamStart","file":"driver/drv_bkcamera.c","requires":"",
	//cmddetail:"examples":"CAM_Start 8080"}
	CMD_RegisterCommand("CAM_Start", CMD_CamStart, NULL);
	//cmddetail:{"name":"CAM_Stop","args":"",
	//cmddetail:"descr":"Stops the MJPEG stream and releases the camera.",
	//cmddetail:"fn":"CMD_CamStop","file":"driver/drv_bkcamera.c","requires":"",
	//cmddetail:"examples":"CAM_Stop"}
	CMD_RegisterCommand("CAM_Stop", CMD_CamStop, NULL);
}

void BKCamera_Stop(void) {
	g_camStop = 1;
}

void BKCamera_AppendInformationToHTTPIndexPage(http_request_t *request) {
	hprintf255(request, "<h5>Camera: %s, %i frames sent</h5>", g_camStatus, g_camFrames);
}

#endif // ENABLE_DRIVER_BKCAMERA
