// Based on the V4L2 video capture example
// from Video for Linux Two API Specification
// http://linuxtv.org/downloads/v4l-dvb-apis/capture-example.html
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <getopt.h>             // getopt_long()

#include <fcntl.h>              // low-level i/o
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>           // for gettimeofday()
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>

#include "encode.h"
#include "lib.h"

#define CLEAR(x) memset(&(x), 0, sizeof(x))

struct buffer {
  void   *start;
  size_t  length;
};

static char   *dev_name;
static char   *camera;
static int    channel;
static char   *format;
static v4l2_std_id std_id;
static char   *palette;
static uint32_t pixelformat;
static int    width;
static int    height;
static int    fps;

static int     fd = -1;   // device - file descriptor
struct buffer *buffers;
static int     img_size;

static void open_device(void)
{
  struct stat st;

  if (-1 == stat(dev_name, &st)) {
    fprintf(stderr, "Cannot identify '%s': %d, %s\n",
            dev_name, errno, strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (!S_ISCHR(st.st_mode)) {
    fprintf(stderr, "%s is no device\n", dev_name);
    exit(EXIT_FAILURE);
  }

  fd = open(dev_name, O_RDWR /* required */ | O_NONBLOCK, 0);

  if (-1 == fd) {
    fprintf(stderr, "Cannot open '%s': %d, %s\n",
            dev_name, errno, strerror(errno));
    exit(EXIT_FAILURE);
  }
}

static void close_device(void)
{
  if (-1 == close(fd))
    errno_exit("close_device");

  fd = -1;
}

static void init_read()
{
  buffers = (struct buffer*) calloc(1, sizeof(*buffers));

  if (!buffers) {
    fprintf(stderr, "Out of memory\n");
    exit(EXIT_FAILURE);
  }

  buffers[0].length = img_size;
  buffers[0].start = malloc(img_size);

  if (!buffers[0].start) {
    fprintf(stderr, "Out of memory\n");
    exit(EXIT_FAILURE);
  }
}

static void init_device(void)
{
  struct v4l2_capability cap;
  struct v4l2_format fmt;

  if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
    if (EINVAL == errno) {
      fprintf(stderr, "%s is no V4L2 device\n", dev_name);
      exit(EXIT_FAILURE);
    } else {
      errno_exit("VIDIOC_QUERYCAP");
    }
  }

  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
    fprintf(stderr, "%s is no video capture device\n", dev_name);
    exit(EXIT_FAILURE);
  }

  CLEAR(fmt);
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = width;
  fmt.fmt.pix.height      = height;
  fmt.fmt.pix.pixelformat = pixelformat;
  //fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;

  if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
          errno_exit("VIDIOC_S_FMT");
  /* Note VIDIOC_S_FMT may change width and height. */

  /* Buggy driver paranoia. */
  /*
  min = fmt.fmt.pix.width * 2;
  if (fmt.fmt.pix.bytesperline < min)
          fmt.fmt.pix.bytesperline = min;
  min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
  if (fmt.fmt.pix.sizeimage < min)
          fmt.fmt.pix.sizeimage = min;
  */

  img_size = fmt.fmt.pix.sizeimage;
  if (!buffers) {
    init_read();
    init_encode(buffers[0].start, palette);
  }

  //
  // Select the video input
  // http://linuxtv.org/downloads/v4l-dvb-apis/video.html
  //
  if (-1 == xioctl (fd, VIDIOC_S_INPUT, &channel)) {
    perror ("VIDIOC_S_INPUT");
    exit (EXIT_FAILURE);
  }

  //
  // Selec a new video standard
  //
  struct v4l2_input input;

  memset(&input, 0, sizeof(input));

  input.index = channel;

  if (-1 == xioctl (fd, VIDIOC_ENUMINPUT, &input)) {
    perror ("VIDIOC_ENUM_INPUT");
    exit (EXIT_FAILURE);
  }

  if (0 == (input.std & std_id)) {
    fprintf (stderr, "Format is not supported.\n");
    exit (EXIT_FAILURE);
  }

  if (-1 == ioctl (fd, VIDIOC_S_STD, &std_id)) {
    perror ("VIDIOC_S_STD");
    exit (EXIT_FAILURE);
  }
}

static void process_image(const void *p, int size)
{
  char file[80];
  char cmd[80];
  sprintf(file, "/dev/shm/cam%s.jpg", camera);
  sprintf(cmd, "J%s", camera);

  encode2jpeg(file);
  fprintf(stdout, cmd);
  fflush(stdout);

  fflush(stderr);
}

static int read_frame(void)
{
  if (-1 == read(fd, buffers[0].start, buffers[0].length)) {
    switch (errno) {
    case EAGAIN:
      return 0;
      //errno_exit("EAGAIN read");

    case EIO:
      /* Could ignore EIO, see spec. */
      /* fall through */

    default:
      errno_exit("read_frame");
    }
  }

  process_image(buffers[0].start, buffers[0].length);
  return 1;
}

static void mainloop(void)
{
  //unsigned int count = frame_count;
  struct timeval t1, t2, tv;
  int r;

  while (1) {
    acquire_file_lock(fd, 10);
    init_device();

    // get start time
    gettimeofday(&t1, NULL);

    for (;;) {
      fd_set fds;

      FD_ZERO(&fds);
      FD_SET(fd, &fds);

      /* Timeout. */
      tv.tv_sec = 2;
      tv.tv_usec = 0;

      r = select(fd + 1, &fds, NULL, NULL, &tv);

      if (-1 == r) {
        if (EINTR == errno) {
          continue;
        }
        errno_exit("mainloop: select");
      }

      if (0 == r) {
        fprintf(stderr, "select timeout\n");
        exit(EXIT_FAILURE);
      }

      if (read_frame()) {
        break;
      }
      /* EAGAIN - continue select loop. */
    } // for

    release_file_lock(fd);

    // get end time
    gettimeofday(&t2, NULL);

    //fprintf(stderr, "get_elapsed_ms: %d ms\n", get_elapsed_ms(t1, t2));
    //fflush(stderr);
    xsleep(0, 1000/fps - get_elapsed_ms(t1, t2));
  } // while (1)
}

static void usage(FILE *fp, int argc, char **argv)
{
  fprintf(fp,
      "Usage: %s [options]\n\n"
      "Version 0.1\n"
      "Options:\n"
      "-c | --camera        Camera [%s]\n"
      "-d | --device        Device [%s]\n"
      "-i | --input         Input channel [%d]\n"
      "-f | --format        Format [%s]\n"
      "-p | --palette       Palette [%s]\n"
      "-w | --width         Width [%d]\n"
      "-e | --height        Height [%d]\n"
      "-s | --fps           FPS [%d]\n"
      "-h | --help          Print this message\n"
      "",
      argv[0], camera, dev_name, channel, format, palette, width, height, fps);
}

static const char short_options[] = "c:d:i:f:p:w:e:s:h";

static const struct option
long_options[] = {
        { "camera",  required_argument, NULL, 'c' },
        { "device",  required_argument, NULL, 'd' },
        { "input",   required_argument, NULL, 'i' },
        { "format",  required_argument, NULL, 'f' },
        { "palette", required_argument, NULL, 'p' },
        { "width",   required_argument, NULL, 'w' },
        { "height",  required_argument, NULL, 'e' },
        { "fps",     required_argument, NULL, 's' },
        { "help",    no_argument,       NULL, 'h' },
        { 0, 0, 0, 0 }
};

int main(int argc, char *argv[])
{
  for (;;) {
    int idx;
    int c;

    c = getopt_long(argc, argv, short_options, long_options, &idx);

    if (-1 == c)
      break;

    switch (c) {
    case 0: /* getopt_long() flag */
        break;

    case 'c':
      camera = optarg;
      break;

    case 'd':
      dev_name = optarg;
      break;

    case 'i':
      channel = atoi(optarg);
      break;

    case 'f':
      format = optarg;
      if (strcmp(format,"PAL_M") == 0) {
        std_id = V4L2_STD_PAL_M;
      } else {
        std_id = V4L2_STD_NTSC;  // default!
      }
      break;

    case 'p':
      palette = optarg;
      if (strcmp(palette,"BGR32") == 0) {
        pixelformat = V4L2_PIX_FMT_BGR32;
      } else if (strcmp(palette,"RGB24") == 0) {
        pixelformat = V4L2_PIX_FMT_RGB24;
      } else if (strcmp(palette,"RGB32") == 0) {
        pixelformat = V4L2_PIX_FMT_RGB32;
      } else if (strcmp(palette,"YUYV") == 0) {
        pixelformat = V4L2_PIX_FMT_YUYV;
      } else if (strcmp(palette,"YUV420") == 0) {
        pixelformat = V4L2_PIX_FMT_YUV420;
      } else if (strcmp(palette,"GREY") == 0) {
        pixelformat = V4L2_PIX_FMT_GREY;
      } else {
        pixelformat = V4L2_PIX_FMT_BGR24;  // default!
      }
      break;

    case 'w':
      width = atoi(optarg);
      break;

    case 'e':
      height = atoi(optarg);
      break;

    case 's':
      fps = atoi(optarg);
      break;

    case 'h':
      usage(stdout, argc, argv);
      exit(EXIT_SUCCESS);

    default:
      usage(stderr, argc, argv);
      exit(EXIT_FAILURE);
    }
  }

  open_device();

  mainloop();

  uninit_encode();
  close_device();

  //fprintf(stderr, "\n");
  return 0;
}
