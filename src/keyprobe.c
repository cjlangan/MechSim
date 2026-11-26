
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/uinput.h>

static double now_ms(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec*1000.0+ts.tv_nsec/1e6;}

int main(int argc,char**argv){
  int n = argc>1?atoi(argv[1]):20;
  int gap = argc>2?atoi(argv[2]):180;   /* ms between keys */
  int fd=open("/dev/uinput",O_WRONLY|O_NONBLOCK);
  if(fd<0){perror("open /dev/uinput");return 1;}
  ioctl(fd,UI_SET_EVBIT,EV_KEY);
  for(int k=0;k<256;k++) ioctl(fd,UI_SET_KEYBIT,k);
  struct uinput_setup us; memset(&us,0,sizeof us);
  us.id.bustype=BUS_USB; us.id.vendor=1; us.id.product=1; us.id.version=1;
  strcpy(us.name,"mechprobe-kbd");
  if(ioctl(fd,UI_DEV_SETUP,&us)<0){perror("setup");return 1;}
  if(ioctl(fd,UI_DEV_CREATE)<0){perror("create");return 1;}
  int warm = argc>3?atoi(argv[3]):300;
  usleep(warm*1000);
  fprintf(stderr,"READY\n"); fflush(stderr);
  struct input_event ev;
  for(int i=0;i<n;i++){
    memset(&ev,0,sizeof ev);
    ev.type=EV_KEY; ev.code=KEY_A; ev.value=1;
    double t=now_ms();
    write(fd,&ev,sizeof ev);
    memset(&ev,0,sizeof ev); ev.type=EV_SYN; ev.code=SYN_REPORT; ev.value=0;
    write(fd,&ev,sizeof ev);
    printf("KEY %.3f\n",t); fflush(stdout);
    usleep(20*1000);
    memset(&ev,0,sizeof ev); ev.type=EV_KEY; ev.code=KEY_A; ev.value=0;
    write(fd,&ev,sizeof ev);
    memset(&ev,0,sizeof ev); ev.type=EV_SYN; ev.code=SYN_REPORT; ev.value=0;
    write(fd,&ev,sizeof ev);
    usleep((gap-20)*1000);
  }
  printf("END %.3f\n", now_ms());
  ioctl(fd,UI_DEV_DESTROY); close(fd);
  return 0;
}
