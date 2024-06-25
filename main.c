#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <error.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <linux/uinput.h>
#include <linux/input.h>

typedef struct Split {
   char *data;
   int *indices;
   size_t data_length;
   size_t indices_length;
} Split;

typedef struct ThreadArguments {
   int fd;
   int key;
   ssize_t *counter;
   int *running;
} ThreadArguments;

static inline void emit(int fd, int type, int code, int val);
static inline void click(int fd, const int key, ssize_t *counter);
static char *exec_command(const char *command);
static Split split(char *in, const char key);
static inline char *split_get(const Split split, const int idx);
static void *thread_task(void *_args);

int main(void)
{
   int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

   /*
    * The ioctls below will enable the device that is about to be
    * created, to pass key events, in this case the space key and the left mouse button
    */
   ioctl(fd, UI_SET_EVBIT, EV_KEY);
   ioctl(fd, UI_SET_KEYBIT, KEY_SPACE);
   ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);

   struct uinput_setup usetup;
   memset(&usetup, 0, sizeof(usetup));
   usetup.id.bustype = BUS_USB;
   usetup.id.vendor = 0x1234; /* sample vendor */
   usetup.id.product = 0x5678; /* sample product */
   strcpy(usetup.name, "Fast Clicker");

   ioctl(fd, UI_DEV_SETUP, &usetup);
   ioctl(fd, UI_DEV_CREATE);

   /* Get keyboard devices */
   char *res = exec_command("grep -B8 -E 'KEY=.*e$' /proc/bus/input/devices");

   Split s = split(res, '\n');

   char *keyboard_devices[10];
   int k = 0;
   for (int j = 5; j < s.indices_length - 1; j += 10) {
      char *handlers = split_get(s, j);

      char *start = strstr(handlers, "event");
      char *end = start;
      int len = 0;
      while (*end > ' ') {
         end++; 
         len++;
      }

      char *ev = calloc(12+len, sizeof(char));
      strcpy(ev, "/dev/input/");
      memcpy(ev+11, start, len);

      keyboard_devices[k] = ev; 
      k++;

      free(handlers);
   }

   /* Get device file descriptors */
   struct input_event ev;
   int device_descriptors[k];
   int d = 0;
   for (int i = 0; i < k; i++) {
      printf("%s\n", keyboard_devices[i]);
      int input_fd = open(keyboard_devices[i], O_RDONLY);
      if (input_fd == -1) {
         printf("failed to open %s\n", keyboard_devices[i]);
         continue;
      }
      device_descriptors[d] = input_fd;
      d++;
   }

   /* Set the device descriptor to non-blocking mode */
   for (int i = 0; i < d; i++) {
      int fd = device_descriptors[i];

      int flags = fcntl(fd, F_GETFL, 0);
      if (flags == -1) perror("failed to get file flags");

      int err = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      if (err == -1) perror("failed to set non-blocking mode");
   }
   
   puts("q to quit");

   int running = 1;
   int clicking = 0;
   ssize_t counter = 0;

   pthread_t thread;
   ThreadArguments args = {
      .fd = fd,
      .key = BTN_LEFT,
      .counter = &counter,
      .running = &clicking,
   };

   while (running) {
      for (int i = 0; i < d; i++) {
         ssize_t err = read(device_descriptors[i], &ev, sizeof(ev));
         if (err == (ssize_t) -1) continue;
         if (ev.type == EV_KEY) {
            if (ev.code == KEY_E) {
               if (ev.value == 1) {
                  clicking = 1;
                  if (pthread_create(&thread, NULL, thread_task, &args) != 0) 
                     perror("Failed to create thread");
               } else if (ev.value == 0) {
                  clicking = 0;
                  //if (pthread_join(thread, NULL) != 0) 
                  //   perror("Failed to join thread");
               }
            }
            if (ev.code == KEY_Q) {
               if (ev.value == 0) running = 0;
            }
         }
      }
   }
   
   /* Cleanup */
   ioctl(fd, UI_DEV_DESTROY);
   close(fd);

   for (int i = 0; i < k; i++) free(keyboard_devices[i]);
   for (int i = 0; i < d; i++) close(device_descriptors[i]);

   free(s.data);
   free(s.indices);

   return 0;
}

/* 
 * Read the bitmask of each device. 
 * If a bitmask ends with 'e', it supports KEY_2, KEY_1, KEY_ESC, and KEY_RESERVED is set to 0, so it's probably a keyboard
 * keybit:   https://github.com/torvalds/linux/blob/02de58b24d2e1b2cf947d57205bd2221d897193c/include/linux/input.h#L45
 * keycodes: https://github.com/torvalds/linux/blob/139711f033f636cc78b6aaf7363252241b9698ef/include/uapi/linux/input-event-codes.h#L75
 * grep -B8 -E 'KEY=.*e$' /proc/bus/input/devices
 */
static inline void emit(int fd, int type, int code, int val)
{
   struct input_event ev;
   ev.type = type;
   ev.code = code;
   ev.value = val;
   /* timestamp values below are ignored */
   ev.time.tv_sec = 0;
   ev.time.tv_usec = 0;

   write(fd, &ev, sizeof(ev));
}

static inline void click(int fd, const int key, ssize_t *counter) {
   emit(fd, EV_KEY, BTN_LEFT, 1);
   emit(fd, EV_SYN, SYN_REPORT, 0);
   emit(fd, EV_KEY, BTN_LEFT, 0);
   emit(fd, EV_SYN, SYN_REPORT, 0); 
   *counter += 1;
}

static char *exec_command(const char *command)
{
   const size_t BUFFER_SIZE = 3000;
   char *buffer = (char *)malloc(BUFFER_SIZE);
   memset(buffer, 0, BUFFER_SIZE);

   FILE *pipe = popen(command, "r");
   if (!pipe) {
      perror("popen failed");
      return NULL;
   }

   size_t t = fread(buffer, sizeof(char), BUFFER_SIZE, pipe);

   if (pclose(pipe) == -1) {
      perror("pclose failed");
   }

   return buffer;
}

static Split split(char *in, const char key) 
{
   size_t BUFFER_SIZE = 100 * sizeof(int);
   int *indices = (int *)malloc(BUFFER_SIZE);

   indices[0] = 0;
   size_t indices_length = 1;

   int i = 1;
   for (char *pointer = in; *pointer != '\0'; pointer++) {
      if (*pointer == key) {
         if (indices_length >= BUFFER_SIZE) indices = (int *)realloc(indices, BUFFER_SIZE * 2);
         indices[indices_length] = i;
         indices_length++;
      }
      i++;
   }
   if (indices_length >= BUFFER_SIZE) indices = (int *)realloc(indices, BUFFER_SIZE * 2);
   indices[indices_length] = i;
   indices_length++;

   Split ret = {
      .data = in,
      .indices = indices,
      .data_length = i-1,
      .indices_length = indices_length,
   };

   return ret;
}
 
static inline char *split_get(const Split split, const int idx)
{
   const size_t len = split.indices[idx+1] - split.indices[idx] - 1;
   if (len <= 0) {
      return NULL;
   }

   /* +1 for null terminator */
   char *ret = calloc(len+1, sizeof(char));
   memcpy(ret, split.data+split.indices[idx], len);
   return ret;
}

static void *thread_task(void *_args) {
   ThreadArguments *args = (ThreadArguments *)_args;

   struct timespec req;
   req.tv_sec = 0;
   //req.tv_nsec = 500000000L; // 0.5 seconds
   req.tv_nsec = 50000000L; // 500 miliseconds

   while (*(args->running)) {
      click(args->fd, args->key, args->counter);
      nanosleep(&req, NULL);
   }

   printf("\n %li Clicks\n", *args->counter);

   return NULL;
}

