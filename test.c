#include<stdio.h>
#include<stdlib.h>
#include<errno.h>
#include<fcntl.h>
#include<string.h>
#include<unistd.h>
#include<sys/ioctl.h>
#include<stdint.h>

#define RD_GPIOLED _IOR('a',1,uint32_t)
#define WR_BLINKPRD _IOW('a',2,uint8_t)
#define INIT_LCD _IOWR('a',3,uint8_t)
#define WR_LCD _IOW('a',4,uint8_t)

#define BUFFER_LENGTH 256               ///< The buffer length (crude but fine)
static char receive[BUFFER_LENGTH];     ///< The receive buffer from the LKM

int main(){
   int ret, fd,sel;
   int32_t value;
   unsigned int num = 0;
   char stringToSend[BUFFER_LENGTH];
   printf("Starting device test code example...\n");
   fd = open("/dev/ebbchar", O_RDWR);             // Open the device with read/write access
   if (fd < 0){
      perror("Failed to open the device...");
      return errno;
   }
   printf("Reading Value from Driver\n");
   ioctl(fd, RD_GPIOLED, (int32_t*) &value);
   printf("contrast_ctl pin: GPIO_%d\n", value);
   printf("set_reset pin: GPIO_49\n");
   printf("Enable pin: GPIO_48\n");
   while(1) {
   printf("please select menu:\n1. Initialize lcd \n2. Show last message\n3. Send message \n4. Exit\n");
   scanf("%d",&sel);
   switch(sel){
    /* case 1:
	printf("Enter the contrast-rate: ");
        scanf("%d",&num);
        ioctl(fd,WR_BLINKPRD,num);
	break; */
    case 1:
        ioctl(fd,INIT_LCD,0);
	break;
    case 2:
        ioctl(fd,WR_LCD,0);
        break;
    case 3:
	getchar();
	printf("Enter your message: ");
	scanf("%[^\n]%*c",&stringToSend);
	printf("Writing message to the device [%s].\n", stringToSend);
   	ret = write(fd, stringToSend, strlen(stringToSend)); // Send the string to the LKM
   	if (ret < 0){
      	perror("Failed to write the message to the device.");
      	return errno;
	}
	break;	
    case 4:
	goto end;
   default:
	break;
  }
}
end:   printf("Closing Driver\n");
       close(fd);
 
   return 0;
}
