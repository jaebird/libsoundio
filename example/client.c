/*
    Simple udp client
*/
#include<stdio.h> //printf
#include<string.h> //memset
#include<stdlib.h> //exit(0);
#include<arpa/inet.h>
#include<sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
 
#define SERVER "127.0.0.1"
//#define SERVER "192.168.56.102"
#define BUFLEN 512  //Max length of buffer
#define PORT 8888   //The port on which to send data
 
void die(char *s)
{
    perror(s);
    exit(1);
}
 
int main(void)
{
    struct sockaddr_in si_other;
    int the_socket, i, slen=sizeof(si_other);
    char buf[BUFLEN];
    char message[BUFLEN];
    int pollingDelay = 500;
 
    if ( (the_socket=socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
        die("socket");
    }
 
    memset((char *) &si_other, 0, sizeof(si_other));
    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(PORT);
     
    if (inet_aton(SERVER , &si_other.sin_addr) == 0) 
    {
        fprintf(stderr, "inet_aton() failed\n");
        exit(1);
    }
 
    while(1)
    {
        //printf("Enter message : ");
        //gets(message);
        sprintf(message, "PING");
        puts("PING");
         
        //send the message
        if (sendto(the_socket, message, strlen(message) , 0 , (struct sockaddr *) &si_other, slen)==-1)
        {
            die("sendto()");
        }

      uint len = 0;

      // check for existing data on the socket
      ioctl(the_socket, FIONREAD, &len);

      if (len > 0) {
        //receive a reply and print it
        //clear the buffer by filling null, it might have previously received data
        memset(buf,'\0', BUFLEN);
        //try to receive some data, this is a blocking call
        if (recvfrom(the_socket, buf, BUFLEN, 0, (struct sockaddr *) &si_other, &slen) == -1)
        {
            die("recvfrom()");
        }
         
        puts(buf);
      } else {
        usleep(pollingDelay*1000);  /* sleep for 100 milliSeconds */
      }

    }
 
    close(the_socket);
    return 0;
}
