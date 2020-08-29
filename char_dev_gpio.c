#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h> 
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h> 
#include <linux/gpio.h>       // Required for the GPIO functions
#include <linux/kobject.h>    // Using kobjects for the sysfs bindings
#include <linux/kthread.h>    // Using kthreads for the flashing functionality
#include <linux/delay.h> 
#include <linux/ioctl.h>
#include <linux/interrupt.h>

#define DEVICE_NAME "ebbchar"
#define CLASS_NAME "ebb" 

#define RD_contrast_ctl _IOR('a',1,uint32_t)
#define WR_BLINKPRD _IOW('a',2,uint8_t)
#define INIT_LCD _IOWR('a',3,uint8_t)
#define WR_LCD _IOW('a',4,uint8_t)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sajan Jha & Rajan Jha");
MODULE_DESCRIPTION("Linux char driver to operate LED driver LKM for the BBB");
MODULE_VERSION("0.1");

/*------------------------------------ GPIO DETAILS ---------------------------------------------- */
static unsigned int EN   = 48;                                  //LCD ENABLE PIN
static unsigned int RS   = 49;                                   //LCD RS PIN

static unsigned int INT  = 65; 

static unsigned int D0   = 66;                                   //LCD DATA_0 PIN
static unsigned int D1   = 69;                                   //LCD DATA_1 PIN
static unsigned int D2   = 47;                                   //LCD DATA_2 PIN
static unsigned int D3   = 27;                                   //LCD DATA_3 PIN
/*-------------------------------------------------------------------------------*/

#define OFF_STATE 0
#define ON_STATE  1

int var;
static int irq_line;

/*--------------------------------- Char Dev---------------------------------------------*/
static int    majorNumber;                  ///< Stores the device number -- determined automatically
static char   message[256] = {0};           ///< Memory for the string that is passed from userspace
static short  size_of_message;              ///< Used to remember the size of the string stored
static int    numberOpens = 0;              ///< Counts the number of times the device is opened
static struct class*  ebbcharClass  = NULL; ///< The device-driver class struct pointer
static struct device* ebbcharDevice = NULL; ///< The device-driver device struct pointer

// The prototype functions for the character driver -- must come before the struct definition
static int     dev_open(struct inode *, struct file *);
static int     dev_release(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static long chr_ioctl(struct file*,unsigned int, unsigned long);
void lcd_string(unsigned char *);
void clear_lcd(void);
int alarm_pitched = 0;
static struct file_operations fops =
{
   .open = dev_open,
   .read = dev_read,
   .write = dev_write,
   .release = dev_release,
   .unlocked_ioctl = chr_ioctl,
};

int32_t temp = 0;
//----------------------------------------------------Char Dev

static unsigned int contrast_ctl = 60; 
module_param(contrast_ctl, uint, S_IRUGO); 
MODULE_PARM_DESC(contrast_ctl, " GPIO LED number (default=49)");  

static unsigned int blinkPeriod = 75;
module_param(blinkPeriod, uint, S_IRUGO);
MODULE_PARM_DESC(blinkPeriod, " LED blink period in ms (min=1, default=100, max=100)");

static char Contrast_ctlName[7] = "gpioXXX"; 
static bool ledOn = 0; 
enum modes { OFF, ON, FLASH }; 
static enum modes mode = FLASH; 

static irqreturn_t irq_handler(int irq, void *dev_id)
{
  static int a =0;
  if(!gpio_get_value(INT)) {
    a++;
    if(a >= 1){ var = 9; a = 0; }	
  }
  printk("In the Interrupt handler,: %d\n",gpio_get_value(INT));
  return IRQ_HANDLED;
}


static ssize_t mode_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
   switch(mode){
      case OFF:   return sprintf(buf, "off\n");       // Display the state -- simplistic approach
      case ON:    return sprintf(buf, "on\n");
      case FLASH: return sprintf(buf, "flash\n");
      default:    return sprintf(buf, "LKM Error\n"); // Cannot get here
   }
}

static ssize_t mode_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
   /* the count-1 is important as otherwise the \n is used in the comparison */
   if (strncmp(buf,"on",count-1)==0) { mode = ON; }   // strncmp() compare with fixed number chars
   else if (strncmp(buf,"off",count-1)==0) { mode = OFF; }
   else if (strncmp(buf,"flash",count-1)==0) { mode = FLASH; }
   return count;
}

static ssize_t period_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
   return sprintf(buf, "%d\n", blinkPeriod);
}

static ssize_t period_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
   unsigned int period;                     // Using a variable to validate the data sent
   sscanf(buf, "%du", &period);             // Read in the period as an unsigned int
   if ((period>24)&&(period<=100)){        // Must be 2ms or greater, 10secs or less
      blinkPeriod = period;                 // Within range, assign to blinkPeriod variable
   }
   return period;
}

static struct kobj_attribute period_attr = __ATTR(blinkPeriod, 0660, period_show, period_store);
static struct kobj_attribute mode_attr = __ATTR(mode, 0660, mode_show, mode_store);

static struct attribute *ebb_attrs[] = {
   &period_attr.attr,                       // The period at which the LED flashes
   &mode_attr.attr,                         // Is the LED on or off?
   NULL,
};

static struct attribute_group attr_group = {
   .name  = Contrast_ctlName,                        // The name is generated in ebbLED_init()
   .attrs = ebb_attrs,                      // The attributes array defined just above
};

static struct kobject *ebb_kobj;            /// The pointer to the kobject
static struct task_struct *task;            /// The pointer to the thread task

static int flash(void *arg){
   static int count  = 0;
   printk(KERN_INFO "EBB LED: Thread has started running \n");
   while(!kthread_should_stop()){           // Returns true when kthread_stop() is called
      set_current_state(TASK_RUNNING);
      if (mode==FLASH) ledOn = !ledOn;      // Invert the LED state
      else if (mode==ON) ledOn = true;
      else ledOn = false;
      gpio_set_value(contrast_ctl, ledOn);       // Use the LED state to light/turn off the LED
      set_current_state(TASK_INTERRUPTIBLE);
      if( ledOn ) {
      msleep(10);                // millisecond sleep for half of the period
      }else { msleep(900); }
      if((var == 9) && alarm_pitched == 0)
      {
        printk(KERN_INFO "into var = 9\n");
	lcd_string(" Warning! ");
	var = 5;
	alarm_pitched  = 1;	
      }
     // printk(KERN_INFO "current gpio int: %d\n",gpio_get_value(INT));
      if(!gpio_get_value(INT) && alarm_pitched)
      {printk(KERN_INFO "into alarm: %d\n",count); 
       count ++; if( count > 7 ) { printk(KERN_INFO "clearing LCD\n"); 
       clear_lcd(); var = 7; count = 0; alarm_pitched = 0;}}
   }
   printk(KERN_INFO "EBB LED: Thread has run to completion \n");
   return 0;
}

//---------------------------------------------INIT Fun Begin-------------------
static int __init ebbchar_init(void){
   
   int result = 0;
   int errno;

   printk(KERN_INFO "EBBChar: Initializing the EBBChar LKM\n");

   /* Try to dynamically allocate a major number for the device -- more difficult but worth it */
   majorNumber = register_chrdev(0, DEVICE_NAME, &fops);
   
  if (majorNumber<0){
      printk(KERN_ALERT "EBBChar failed to register a major number\n");
      return majorNumber;
   }
   printk(KERN_INFO "EBBChar: registered correctly with major number %d\n", majorNumber);

   /* Register the device class */
   ebbcharClass = class_create(THIS_MODULE, CLASS_NAME);
   if (IS_ERR(ebbcharClass)){                // Check for error and clean up if there is
      unregister_chrdev(majorNumber, DEVICE_NAME);
      printk(KERN_ALERT "Failed to register device class\n");
      return PTR_ERR(ebbcharClass);          // Correct way to return an error on a pointer
   }
   printk(KERN_INFO "EBBChar: device class registered correctly\n");

   // Register the device driver
   ebbcharDevice = device_create(ebbcharClass, NULL, MKDEV(majorNumber, 0), NULL, DEVICE_NAME);
   if (IS_ERR(ebbcharDevice)){               // Clean up if there is an error
      class_destroy(ebbcharClass);           // Repeated code but the alternative is goto statements
      unregister_chrdev(majorNumber, DEVICE_NAME);
      printk(KERN_ALERT "Failed to create the device\n");
      return PTR_ERR(ebbcharDevice);
   }
   //---------------------Initializing the EBB LED--------------------
   printk(KERN_INFO "EBB LED: Initializing the EBB LED LKM\n");
   sprintf(Contrast_ctlName, "gpio%d", contrast_ctl);      // Create the gpio115 name for /sys/ebb/led49

   ebb_kobj = kobject_create_and_add("ebb", kernel_kobj->parent); // kernel_kobj points to /sys/kernel
   if(!ebb_kobj){
      printk(KERN_ALERT "EBB LED: failed to create kobject\n");
      return -ENOMEM;
   }
   // add the attributes to /sys/ebb/ -- for example, /sys/ebb/led49/ledOn
   result = sysfs_create_group(ebb_kobj, &attr_group);
   if(result) {
      printk(KERN_ALERT "EBB LED: failed to create sysfs group\n");
      kobject_put(ebb_kobj);                // clean up -- remove the kobject sysfs entry
      return result;
   }
   ledOn = true;
   gpio_request(contrast_ctl, "sysfs");          // contrast_ctl is 60 by default, request it
   gpio_direction_output(contrast_ctl, ledOn);   // Set the gpio to be in output mode and turn on
   gpio_export(contrast_ctl, false);  // causes gpio60 to appear in /sys/class/gpio
                                 // the second argument prevents the direction from being changed
   gpio_direction_output(EN, OFF_STATE);
   gpio_direction_output(RS, OFF_STATE);
   gpio_direction_output(D0, OFF_STATE);
   gpio_direction_output(D1, OFF_STATE);
   gpio_direction_output(D2, OFF_STATE);
   gpio_direction_output(D3, OFF_STATE);
   
   if((errno = gpio_direction_input(INT)) != 0)
   {
    printk(KERN_INFO "Can't set GPIO direction, error %i\n", errno);
    gpio_free(INT);
    return -EINVAL;
   }
   irq_line = gpio_to_irq(INT);
   printk ("IRQ Line is %d \n",irq_line);

   errno = request_irq( irq_line, (irq_handler_t)irq_handler, IRQF_TRIGGER_FALLING , "Interrupt123", NULL );
   if(errno<0)
   {
    printk(KERN_INFO "Problem requesting IRQ, error %i\n", errno);
   }


   task = kthread_run(flash, NULL, "LED_flash_thread");  // Start the LED flashing thread
   if(IS_ERR(task)){                                     // Kthread name is LED_flash_thread
      printk(KERN_ALERT "EBB LED: failed to create the task\n");
      return PTR_ERR(task);
   }
   
   printk(KERN_INFO "EBBChar: Char Dev For led created correctly\n"); // Made it! device was initialized
   return result;
}

/** @brief The LKM cleanup function
 *  Similar to the initialization function, it is static. The __exit macro notifies that if this
 *  code is used for a built-in driver (not a LKM) that this function is not required.
 */
static void __exit ebbchar_exit(void){
   kthread_stop(task);                      // Stop the LED flashing thread
   kobject_put(ebb_kobj);                   // clean up -- remove the kobject sysfs entry
   gpio_set_value(contrast_ctl, OFF_STATE);              // Turn the LED off, indicates device was unloaded

   gpio_set_value(EN, OFF_STATE);
   gpio_set_value(RS, OFF_STATE);
   gpio_set_value(D0, OFF_STATE);
   gpio_set_value(D1, OFF_STATE);
   gpio_set_value(D2, OFF_STATE);
   gpio_set_value(D3, OFF_STATE);
   
   gpio_unexport(contrast_ctl);                  // Unexport the Button GPIO
   gpio_free(contrast_ctl);                     // Free the LED GPIO
   gpio_free(EN);
   gpio_free(RS);
   gpio_free(D0);
   gpio_free(D1);
   gpio_free(D2);
   gpio_free(D3);
   gpio_free(INT);

   free_irq(irq_line,NULL);
   //----Now remove EBB LED LKM
   device_destroy(ebbcharClass, MKDEV(majorNumber, 0));     // remove the device
   class_unregister(ebbcharClass);                          // unregister the device class
   class_destroy(ebbcharClass);                             // remove the device class
   unregister_chrdev(majorNumber, DEVICE_NAME);             // unregister the major number
   printk(KERN_INFO "EBBChar: Goodbye from the LKM!\n");
}

/** @brief The device open function that is called each time the device is opened
 *  This will only increment the numberOpens counter in this case.
 *  @param inodep A pointer to an inode object (defined in linux/fs.h)
 *  @param filep A pointer to a file object (defined in linux/fs.h)
 */
static int dev_open(struct inode *inodep, struct file *filep){
   numberOpens++;
   printk(KERN_INFO "EBBChar: Device has been opened %d time(s)\n", numberOpens);
   return 0;
}

/** @brief This function is called whenever device is being read from user space i.e. data is
 *  being sent from the device to the user. In this case is uses the copy_to_user() function to
 *  send the buffer string to the user and captures any errors.
 *  @param filep A pointer to a file object (defined in linux/fs.h)
 *  @param buffer The pointer to the buffer to which this function writes the data
 *  @param len The length of the b
 *  @param offset The offset if required
 */
static ssize_t dev_read(struct file *filep, char *buffer, size_t len, loff_t *offset){
   int error_count = 0;
   // copy_to_user has the format ( * to, *from, size) and returns 0 on success
   error_count = copy_to_user(buffer, message, size_of_message);

   if (error_count==0){            // if true then have success
      printk(KERN_INFO "EBBChar: Sent %d characters to the user\n", size_of_message);
      return (size_of_message=0);  // clear the position to the start and return 0
   }
   else {
      printk(KERN_INFO "EBBChar: Failed to send %d characters to the user\n", error_count);
      return -EFAULT;              // Failed -- return a bad address message (i.e. -14)
   }
}

/** @brief This function is called whenever the device is being written to from user space i.e.
 *  data is sent to the device from the user. The data is copied to the message[] array in this
 *  LKM using the sprintf() function along with the length of the string.
 *  @param filep A pointer to a file object
 *  @param buffer The buffer to that contains the string to write to the device
 *  @param len The length of the array of data that is being passed in the const char buffer
 *  @param offset The offset if required
 */
static ssize_t dev_write(struct file *filep, const char *buffer, size_t len, loff_t *offset){
   //sprintf(message, "%s(%zu letters)", buffer, len);   // appending received string with its length
   //size_of_message = strlen(message);                 // store the length of the stored message
   //printk(KERN_INFO "EBBChar: Received %zu characters from the user\n", len);
   size_of_message = copy_from_user(message,buffer,32);
   printk(KERN_INFO "message recieved: %s\n",message);
   lcd_string(message);
   return size_of_message;
}
/*-----------------------------LCD Driver codes----------------------*/

void lcd_cmd (unsigned char cmd)  /* LCD16x2 command funtion */
{
        int i;
        gpio_set_value(RS, OFF_STATE);
        msleep(1);
        for(i=0;i<2;i++)
        {
                if( cmd&0x80 )
                {gpio_set_value(D0,ON_STATE);
                }else{ gpio_set_value(D0,OFF_STATE); }
                cmd <<=1;
                if( cmd&0x80 )
                {gpio_set_value(D1,ON_STATE);
                }else{ gpio_set_value(D1,OFF_STATE); }
                cmd <<=1;
                if( cmd&0x80 )
                {gpio_set_value(D2,ON_STATE);
                }else{ gpio_set_value(D2,OFF_STATE); }
                cmd <<=1;
                if( cmd&0x80 )
                {gpio_set_value(D3,ON_STATE);
                }else{ gpio_set_value(D3,OFF_STATE); }
                cmd <<=1;

                gpio_set_value(EN,ON_STATE);
                msleep(1);
                gpio_set_value(EN,OFF_STATE);
                msleep(5);
        }
}

void lcd_data (unsigned char cmd)  /* LCD16x2 command funtion */
{
        int i;
        gpio_set_value(RS, ON_STATE);
        msleep(1);
        for(i=0;i<2;i++)
        {
                if( cmd&0x80 )
                {gpio_set_value(D0,ON_STATE);
                }else{ gpio_set_value(D0,OFF_STATE); }
                cmd <<=1;
                if( cmd&0x80 )
                {gpio_set_value(D1,ON_STATE);
                }else{ gpio_set_value(D1,OFF_STATE); }
                cmd <<=1;
                if( cmd&0x80 )
                {gpio_set_value(D2,ON_STATE);
                }else{ gpio_set_value(D2,OFF_STATE); }
                cmd <<=1;
                if( cmd&0x80 )
                {gpio_set_value(D3,ON_STATE);
                }else{ gpio_set_value(D3,OFF_STATE); }
                cmd <<=1;

                gpio_set_value(EN,ON_STATE);
                msleep(1);
                gpio_set_value(EN,OFF_STATE);
                msleep(5);
        }
}

void LCD_IN( void )
{
        msleep(100);
        lcd_cmd(0x33);
        msleep(50);
        lcd_cmd(0x32);
        msleep(50);
        lcd_cmd(0x28);
        msleep(20);
        lcd_cmd(0x28);
        msleep(20);
        lcd_cmd(0x0F);
        msleep(20);
        lcd_cmd(0x01);
        msleep(20);
        lcd_cmd(0x06);
        msleep(20);
}
void clear_lcd(void)
{
        lcd_cmd(0x01);
}
void nextLine(void)
{
        lcd_cmd(0xC0);
}
void lcd_string(unsigned char *ch)
{
        int i=0;
        clear_lcd();
        while(*(ch+i))
        {
                lcd_data(*(ch+i));
                i++;
                if( i == 15 && *(ch+(i+1)))
                {
                  nextLine();
                }
        }
}



/*--------------------------- End of LCD driver codes ---------------*/
static long chr_ioctl(struct file* filep,unsigned int cmd, unsigned long arg)
{
        printk(KERN_INFO "reached ioctl\n");
        switch(cmd){
                case RD_contrast_ctl:
                        printk(KERN_INFO "Into read: %d\n",contrast_ctl);
                        if (copy_to_user((uint32_t*)arg,&contrast_ctl,sizeof(contrast_ctl)))
                        {
                                return -EACCES;
                        }
                        break;
                case WR_BLINKPRD:
                        printk(KERN_INFO "Into write: %d\n",(uint32_t)arg);
                        if( 24 < (int32_t)arg && (int32_t)arg <=100 )
                        {
                        printk(KERN_INFO "Into the range\n");
                        memcpy((void*)&temp,(const void*)&arg,sizeof(temp));
                        printk(KERN_INFO "copied into temp: %d\n",temp);
                        blinkPeriod = temp;
                        }else{ 
                                printk(KERN_INFO "Range is off limit ( 24 < range < 100 \n");
                                return -EINVAL; 
                        } 
                        break;
                case INIT_LCD:
                        printk(KERN_INFO "Into LCD initialization\n");
                        LCD_IN();
                        printk(KERN_INFO "LCD initialization completes!\n");
                        lcd_string(" Welcome to lcd ");
                        msleep(1000);
                        lcd_string("    Hi,Sajan    ");
                        msleep(1000);
                        clear_lcd(); 
                        break;
                case WR_LCD:
                        printk(KERN_INFO "Writing Text\n");
                        lcd_string(message);
                        printk(KERN_INFO "Writing done: %s\n",message);
                        break;
                default:
                        return -EINVAL;
        }
        return 0;
}

/** @brief The device release function that is called whenever the device is closed/released by
 *  the userspace program
 *  @param inodep A pointer to an inode object (defined in linux/fs.h)
 *  @param filep A pointer to a file object (defined in linux/fs.h)
 */
static int dev_release(struct inode *inodep, struct file *filep){
   printk(KERN_INFO "EBBChar: Device successfully closed\n");
   return 0;
}

/** @brief A module must use the module_init() module_exit() macros from linux/init.h, which
 *  identify the initialization function at insertion time and the cleanup function (as
 *  listed above)
 */
module_init(ebbchar_init);
module_exit(ebbchar_exit);


