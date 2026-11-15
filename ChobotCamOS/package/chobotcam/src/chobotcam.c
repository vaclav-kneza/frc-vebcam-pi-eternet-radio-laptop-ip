#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/videodev2.h>
#include <linux/gpio.h>
#include <time.h>
#include <poll.h>
#include <netinet/tcp.h>

#define CAMS 2
#define MAXBUFS 3
#define MAXFRAME (1024*1024)
#define BOUNDARY "chobotframe"
static volatile sig_atomic_t running=1;
static int width=320,height=240,fps=20,port=5800,gpio=17;
static char *devs[2]={"/dev/video0","/dev/video2"};
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t updated=PTHREAD_COND_INITIALIZER;
static unsigned char frames[2][MAXFRAME];
static size_t sizes[2];
static uint64_t serials[2];
static int selected=0;
static int gpio_fd=-1, gpio_line_fd=-1;
static uint64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static uint64_t frame_time[2];
static void on_signal(int s){(void)s;running=0;}
static int xioctl(int fd,unsigned long req,void *arg){int r;do{r=ioctl(fd,req,arg);}while(r<0&&errno==EINTR);return r;}
static void gpio_init(void){
 gpio_fd=open("/dev/gpiochip0",O_RDONLY|O_CLOEXEC);
 if(gpio_fd<0){perror("GPIO open (default camera 0)");return;}
 struct gpio_v2_line_request req={0};
 req.offsets[0]=gpio;req.num_lines=1;
 snprintf(req.consumer,sizeof(req.consumer),"chobotcam");
 req.config.flags=GPIO_V2_LINE_FLAG_INPUT;
 if(xioctl(gpio_fd,GPIO_V2_GET_LINE_IOCTL,&req)<0){perror("GPIO request");close(gpio_fd);gpio_fd=-1;return;}
 gpio_line_fd=req.fd;
}
static void update_gpio(void){
 if(gpio_line_fd<0)return;
 struct gpio_v2_line_values v={.mask=1};
 if(xioctl(gpio_line_fd,GPIO_V2_LINE_GET_VALUES_IOCTL,&v)==0){
  int new_selected=(v.bits&1)?1:0;
  if(new_selected!=selected){pthread_mutex_lock(&lock);selected=new_selected;pthread_cond_broadcast(&updated);pthread_mutex_unlock(&lock);}
 }
}
struct mmapbuf{void *addr;size_t len;};
struct camctx{int idx;};
static void *camera_thread(void *arg){
 int idx=((struct camctx*)arg)->idx; struct mmapbuf buffers[MAXBUFS]={0};
 while(running){
  int fd=open(devs[idx],O_RDWR|O_NONBLOCK|O_CLOEXEC);
  if(fd<0){fprintf(stderr,"camera%d: %s: %s\n",idx,devs[idx],strerror(errno));sleep(2);continue;}
  struct v4l2_format fmt={0};fmt.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width=width;fmt.fmt.pix.height=height;
  fmt.fmt.pix.pixelformat=V4L2_PIX_FMT_MJPEG;fmt.fmt.pix.field=V4L2_FIELD_ANY;
  if(xioctl(fd,VIDIOC_S_FMT,&fmt)<0||fmt.fmt.pix.pixelformat!=V4L2_PIX_FMT_MJPEG){
   fprintf(stderr,"camera%d needs hardware MJPEG at %dx%d; skipping\n",idx,width,height);close(fd);sleep(3);continue;
  }
  struct v4l2_streamparm parm={0};parm.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe.numerator=1;parm.parm.capture.timeperframe.denominator=fps;
  xioctl(fd,VIDIOC_S_PARM,&parm);
  struct v4l2_requestbuffers req={0};req.count=2;req.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;req.memory=V4L2_MEMORY_MMAP;
  if(xioctl(fd,VIDIOC_REQBUFS,&req)<0||req.count<2){close(fd);sleep(2);continue;}
  int count=req.count<MAXBUFS?req.count:MAXBUFS,good=1;
  for(int i=0;i<count;i++){
   struct v4l2_buffer b={0};b.type=req.type;b.memory=req.memory;b.index=i;
   if(xioctl(fd,VIDIOC_QUERYBUF,&b)<0){good=0;break;}
   buffers[i].len=b.length;buffers[i].addr=mmap(NULL,b.length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,b.m.offset);
   if(buffers[i].addr==MAP_FAILED){buffers[i].addr=NULL;good=0;break;}
   if(xioctl(fd,VIDIOC_QBUF,&b)<0){good=0;break;}
  }
  enum v4l2_buf_type type=V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if(good&&xioctl(fd,VIDIOC_STREAMON,&type)<0)good=0;
  if(good)fprintf(stderr,"camera%d active: %s %dx%d MJPEG @ requested %d fps\n",idx,devs[idx],width,height,fps);
  while(good&&running){
   fd_set fds;FD_ZERO(&fds);FD_SET(fd,&fds);struct timeval tv={.tv_sec=1};
   int n=select(fd+1,&fds,NULL,NULL,&tv);if(n<0){if(errno==EINTR)continue;break;}if(!n)continue;
   // Drain all ready frames: never allow older USB frames to accumulate.
   // Only the newest valid frame is published to HTTP clients.
   while(running){
    struct v4l2_buffer b={0};b.type=type;b.memory=V4L2_MEMORY_MMAP;
    if(xioctl(fd,VIDIOC_DQBUF,&b)<0){if(errno!=EAGAIN)good=0;break;}
    if(b.index<(unsigned)count && b.bytesused>4 && b.bytesused<=MAXFRAME){
     const unsigned char *src=buffers[b.index].addr;
     if(src&&src[0]==0xff&&src[1]==0xd8){
      pthread_mutex_lock(&lock);memcpy(frames[idx],src,b.bytesused);sizes[idx]=b.bytesused;
      serials[idx]++;frame_time[idx]=now_ms();
      pthread_cond_broadcast(&updated);pthread_mutex_unlock(&lock);
     }
    }
    if(xioctl(fd,VIDIOC_QBUF,&b)<0){good=0;break;}
   }
  }
  if(good)xioctl(fd,VIDIOC_STREAMOFF,&type);
  for(int i=0;i<count;i++)if(buffers[i].addr)munmap(buffers[i].addr,buffers[i].len);
  close(fd);if(running)sleep(1);
 }
 return NULL;
}
// Any slow TCP client is disconnected quickly rather than queuing stale images.
// No frame is sent after its age reaches 200 ms at start of transfer.
static int writefast(int fd,const void *buf,size_t len){
 const unsigned char *p=buf;
 uint64_t deadline=now_ms()+90;
 while(len){
  ssize_t n=send(fd,p,len,MSG_NOSIGNAL|MSG_DONTWAIT);
  if(n>0){p+=n;len-=n;continue;}
  if(n<0&&errno==EINTR)continue;
  if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){
   int ms=(int)(deadline-now_ms());if(ms<=0||ms>90)return -1;
   struct pollfd f={.fd=fd,.events=POLLOUT};
   if(poll(&f,1,ms)<=0||!(f.revents&POLLOUT))return -1;
   continue;
  }
  return -1;
 }
 return 0;
}
static void *client_thread(void *ptr){
 int fd=*(int*)ptr;free(ptr);
 int yes=1,sendbuf=8192;
 setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));
 setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&sendbuf,sizeof(sendbuf));
 char req[1024]={0};
 // Receive an HTTP request, but do not hang on a nonresponsive client.
 struct pollfd rf={.fd=fd,.events=POLLIN};
 if(poll(&rf,1,300)<=0 || recv(fd,req,sizeof(req)-1,MSG_DONTWAIT)<=0)goto out;
 if(strncmp(req,"GET /status",11)==0){
  char msg[300];pthread_mutex_lock(&lock);int s=selected;uint64_t a=serials[0],b=serials[1];pthread_mutex_unlock(&lock);
  int n=snprintf(msg,sizeof(msg),"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"selected\":%d,\"cam0_frames\":%llu,\"cam1_frames\":%llu}\n",s,(unsigned long long)a,(unsigned long long)b);
  writefast(fd,msg,n);goto out;
 }
 if(strncmp(req,"GET /stream.mjpg",16)!=0 && strncmp(req,"GET / ",6)!=0){
  const char *notfound="HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
  writefast(fd,notfound,strlen(notfound));goto out;
 }
 const char *header="HTTP/1.1 200 OK\r\nCache-Control: no-store, no-cache, must-revalidate\r\nPragma: no-cache\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Type: multipart/x-mixed-replace; boundary=" BOUNDARY "\r\n\r\n";
 if(writefast(fd,header,strlen(header)))goto out;
 unsigned char *local=malloc(MAXFRAME);if(!local)goto out;
 int previous_sel=-1;uint64_t previous_serial=0;
 while(running){
  pthread_mutex_lock(&lock);
  int sel=selected;uint64_t seq=serials[sel];size_t sz=sizes[sel];
  uint64_t age=now_ms()-frame_time[sel];
  if(!sz||age>=200||(sel==previous_sel&&seq==previous_serial)){
   // Wait for new camera frame without polling every client at 5 ms intervals.
   struct timespec ts;clock_gettime(CLOCK_REALTIME,&ts);
   ts.tv_nsec+=20000000; if(ts.tv_nsec>=1000000000){ts.tv_sec++;ts.tv_nsec-=1000000000;}
   pthread_cond_timedwait(&updated,&lock,&ts);
   pthread_mutex_unlock(&lock);continue;
  }
  memcpy(local,frames[sel],sz);pthread_mutex_unlock(&lock);
  char part[160];int n=snprintf(part,sizeof(part),"--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",sz);
  if(writefast(fd,part,n)||writefast(fd,local,sz)||writefast(fd,"\r\n",2))break;
  previous_sel=sel;previous_serial=seq;
 }
 free(local);
 out:close(fd);return NULL;
}
static void *gpio_thread(void *ignored){
 (void)ignored;
 while(running){
  update_gpio();
  // GPIO switching typically detected within 2 ms; capture FPS still limits display.
  usleep(2000);
 }
 return NULL;
}

int main(int argc,char **argv){
 if(argc>1)devs[0]=argv[1];
 if(argc>2)devs[1]=argv[2];
 if(argc>3)fps=atoi(argv[3]);
 if(fps<1||fps>60)fps=20;
 signal(SIGPIPE,SIG_IGN);signal(SIGINT,on_signal);signal(SIGTERM,on_signal);
 gpio_init();pthread_t gt;pthread_create(&gt,NULL,gpio_thread,NULL);pthread_t threads[2];struct camctx ctx[2]={{0},{1}};
 for(int i=0;i<2;i++)pthread_create(&threads[i],NULL,camera_thread,&ctx[i]);
 int server=socket(AF_INET,SOCK_STREAM,0);int yes=1;
 setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
 struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=htonl(INADDR_ANY)};
 if(bind(server,(void*)&addr,sizeof(addr))||listen(server,8)){perror("bind/listen");return 1;}
 fprintf(stderr,"ChobotCam listening on 0.0.0.0:%d (GPIO%d)\n",port,gpio);
 while(running){
  fd_set fds;FD_ZERO(&fds);FD_SET(server,&fds);struct timeval tv={.tv_sec=0,.tv_usec=100000};
  int r=select(server+1,&fds,NULL,NULL,&tv);
  if(r<=0)continue;
  int client=accept(server,NULL,NULL);if(client<0)continue;
  int *p=malloc(sizeof(int));if(!p){close(client);continue;}*p=client;
  pthread_t t;if(pthread_create(&t,NULL,client_thread,p)==0)pthread_detach(t);else{close(client);free(p);}
 }
 close(server);pthread_join(gt,NULL);for(int i=0;i<2;i++)pthread_join(threads[i],NULL);
 if(gpio_line_fd>=0)close(gpio_line_fd);
 if(gpio_fd>=0)close(gpio_fd);
 return 0;
}
