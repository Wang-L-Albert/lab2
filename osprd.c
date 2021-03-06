#include <linux/version.h>
#include <linux/autoconf.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>  /* printk() */
#include <linux/errno.h>   /* error codes */
#include <linux/types.h>   /* size_t */
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/wait.h>
#include <linux/file.h>

#include "spinlock.h"
#include "osprd.h"

/* The size of an OSPRD sector. */
#define SECTOR_SIZE	512

/* This flag is added to an OSPRD file's f_flags to indicate that the file
 * is locked. */
#define F_OSPRD_LOCKED	0x80000

/* eprintk() prints messages to the console.
 * (If working on a real Linux machine, change KERN_NOTICE to KERN_ALERT or
 * KERN_EMERG so that you are sure to see the messages.  By default, the
 * kernel does not print all messages to the console.  Levels like KERN_ALERT
 * and KERN_EMERG will make sure that you will see messages.) */
#define eprintk(format, ...) printk(KERN_NOTICE format, ## __VA_ARGS__)

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("CS 111 RAM Disk");
// ------DONE------------ EXERCISE: Pass your names into the kernel as the module's authors.
MODULE_AUTHOR("Albert Wang and Lauren Yeung");

#define OSPRD_MAJOR	222

/* This module parameter controls how big the disk will be.
 * You can specify module parameters when you load the module,
 * as an argument to insmod: "insmod osprd.ko nsectors=4096" */
static int nsectors = 32;
module_param(nsectors, int, 0);

struct ticketNode {
	int ticketNum;
	struct ticketNode *prev;
	struct ticketNode *next;
};
struct readerNode {
	pid_t pid;
	struct readerNode *prev;
	struct readerNode *next;
};

/* The internal representation of our device. */
typedef struct osprd_info {
	uint8_t *data;                  // The data array. Its size is
	                                // (nsectors * SECTOR_SIZE) bytes.

	osp_spinlock_t mutex;           // Mutex for synchronizing access to
					// this block device

	unsigned ticket_head;		// Currently running ticket for
					// the device lock

	unsigned ticket_tail;		// Next available ticket for
					// the device lock

	wait_queue_head_t blockq;       // Wait queue for tasks blocked on
					// the device lock
	int numReadLocks; //tracks how many files reading from disk, multiple allowed
	int isWriteLocked; //tracks if currently being written to
	pid_t writeLockPid;
	struct ticketNode *ticketListHead;
	struct readerNode *readerListHead;


	/* HINT: You may want to add additional fields to help
	         in detecting deadlock. */

	// The following elements are used internally; you don't need
	// to understand them.
	struct request_queue *queue;    // The device request queue.
	spinlock_t qlock;		// Used internally for mutual
	                                //   exclusion in the 'queue'.
	struct gendisk *gd;             // The generic disk.
} osprd_info_t;

#define NOSPRD 4
static osprd_info_t osprds[NOSPRD];


// Declare useful helper functions

/*
 * file2osprd(filp)
 *   Given an open file, check whether that file corresponds to an OSP ramdisk.
 *   If so, return a pointer to the ramdisk's osprd_info_t.
 *   If not, return NULL.
 */
static osprd_info_t *file2osprd(struct file *filp);

/*
 * for_each_open_file(task, callback, user_data)
 *   Given a task, call the function 'callback' once for each of 'task's open
 *   files.  'callback' is called as 'callback(filp, user_data)'; 'filp' is
 *   the open file, and 'user_data' is copied from for_each_open_file's third
 *   argument.
 */
static void for_each_open_file(struct task_struct *task,
			       void (*callback)(struct file *filp,
						osprd_info_t *user_data),
			       osprd_info_t *user_data);


/*
 * osprd_process_request(d, req)
 *   Called when the user reads or writes a sector.
 *   Should perform the read or write, as appropriate.
 */
static void osprd_process_request(osprd_info_t *d, struct request *req)
{
	if (!blk_fs_request(req)) {
		end_request(req, 0);
		return;
	}

	// -----DONE?------EXERCISE: Perform the read or write request by copying data between
	// our data array and the request's buffer.
	// Hint: The 'struct request' argument tells you what kind of request
	// this is, and which sectors are being read or written.
	// Read about 'struct request' in <linux/blkdev.h>.
	// Consider the 'req->sector', 'req->current_nr_sectors', and
	// 'req->buffer' members, and the rq_data_dir() function.

	// Your code here.

	unsigned int requestType;
	uint8_t *dataPtr;
	int dataSize;
	dataSize = req->current_nr_sectors * SECTOR_SIZE; //size of data to be copied, nr_sectors is num sectors to be copied


	requestType = rq_data_dir(req); //get the request type
	dataPtr = d->data + (req->sector * SECTOR_SIZE); //get a pointer to the location on disk where we need to write

	if(requestType == READ){
		memcpy((void*) req->buffer, (void*) dataPtr, dataSize); //copy from ramdisk to request's buffer
	} else if (requestType == WRITE){
		memcpy((void*) dataPtr, (void*) req->buffer, dataSize); //copy from request's buffer to ramdisk
	} else{
		eprintk("Request of was of an unknown command. \n");
		end_request(req, 0);
	}


	eprintk("Should process request...\n");

	end_request(req, 1);
}


// This function is called when a /dev/osprdX file is opened.
// You aren't likely to need to change this.
static int osprd_open(struct inode *inode, struct file *filp)
{
	// Always set the O_SYNC flag. That way, we will get writes immediately
	// instead of waiting for them to get through write-back caches.
	filp->f_flags |= O_SYNC;
	return 0;
}


// This function is called when a /dev/osprdX file is finally closed.
// (If the file descriptor was dup2ed, this function is called only when the
// last copy is closed.)
static int osprd_close_last(struct inode *inode, struct file *filp)
{
	if (filp) {
		osprd_info_t *d = file2osprd(filp);
		int filp_writable = filp->f_mode & FMODE_WRITE;
		// ------DONE?------EXERCISE: If the user closes a ramdisk file that holds
		// a lock, release the lock.  Also wake up blocked processes
		// as appropriate.

		// Your code here.
		if (filp->f_flags & F_OSPRD_LOCKED){
			//release lock //wake up all tasks blocked by filp holding the lock
			osp_spin_lock(&(d->mutex)); //lock our current ramdisk file to work on
			//eprintk("is write locked:    %d  \n",d->isWriteLocked);
			//release all locks current process holds
			//eprintk("is write locked1:    %d  \n",d->isWriteLocked);
			eprintk("close last: numReadLocks: %d \n", d->numReadLocks);
			if (d->numReadLocks != 0){//clear any read locks associated with process on the file
				struct readerNode *traverse;
				traverse = d->readerListHead;
				while (traverse->next != NULL){
					eprintk("Traversing pid in close last: %d, current pid %d\n", traverse->pid, current->pid);
					//eprintk("TRAVERSALS NEXT NODE'S PID:    %d  \n",traverse->next->pid);
					if(traverse->pid == current->pid){//if node has pid == current->pid, we need to remove from list of readers
						//check if we're at the last
						struct readerNode *temp = traverse; //get a temp to point to current node so we can delete
						if(traverse != d->readerListHead){//if not first node
							traverse->prev->next = traverse->next;//re-link
							traverse->next->prev = traverse->prev;
						} else {
							d->readerListHead = traverse->next;//if is first node
							traverse->next->prev = NULL;
						}
						 //advance traverser
						traverse = traverse->next;
						kfree(temp);
						d->numReadLocks--; //if we've tossed out a readerlist node, that means we have 1 fewer reader
					} else{
						traverse = traverse->next;
						//eprintk("CLOSELAST TRAVERSALS\n");
					}
				}
			}
			//eprintk("is write locked:    %d  \n",d->isWriteLocked);
			//eprintk("writeLockPid:    %d  \n",d->writeLockPid);
			if((d->isWriteLocked) && (d->writeLockPid == current->pid)){//clear the write lock if the process owns it
				d->isWriteLocked = 0;
				d->writeLockPid = -1;
				eprintk("releasing writelock in close last\n");
			}
			if (filp->f_flags & F_OSPRD_LOCKED){//if file is holds a lock
				filp->f_flags &= ~F_OSPRD_LOCKED; //clear the lock filp holds
			}
			osp_spin_unlock(&(d->mutex));
			wake_up_all(&(d->blockq)); //wake up all tasks blocked by filp holding the lock
			//eprintk("CLOSELAST 1111111\n");
		}
	}

	return 0;
}


/*
 * osprd_lock
 */

/*
 * osprd_ioctl(inode, filp, cmd, arg)
 *   Called to perform an ioctl on the named file.
 */
int osprd_ioctl(struct inode *inode, struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	osprd_info_t *d = file2osprd(filp);	// device info
	int r = 0;			// return value: initially 0

	// is file open for writing?
	int filp_writable = (filp->f_mode & FMODE_WRITE) != 0;

	// This line avoids compiler warnings; you may remove it.
	(void) filp_writable, (void) d;

	// Set 'r' to the ioctl's return value: 0 on success, negative on error

	if (cmd == OSPRDIOCACQUIRE) {

		// EXERCISE: Lock the ramdisk.
		//
		// If *filp is open for writing (filp_writable), then attempt
		// to write-lock the ramdisk; otherwise attempt to read-lock
		// the ramdisk.
		//
                // This lock request must block using 'd->blockq' until:
		// 1) no other process holds a write lock;
		// 2) either the request is for a read lock, or no other process
		//    holds a read lock; and
		// 3) lock requests should be serviced in order, so no process
		//    that blocked earlier is still blocked waiting for the
		//    lock.
		//
		// If a process acquires a lock, mark this fact by setting
		// 'filp->f_flags |= F_OSPRD_LOCKED'.  You also need to
		// keep track of how many read and write locks are held:
		// change the 'osprd_info_t' structure to do this.
		//
		// Also wake up processes waiting on 'd->blockq' as needed.
		//
		// If the lock request would cause a deadlock, return -EDEADLK.
		// If the lock request blocks and is awoken by a signal, then
		// return -ERESTARTSYS.
		// Otherwise, if we can grant the lock request, return 0.

		// 'd->ticket_head' and 'd->ticket_tail' should help you
		// service lock requests in order.  These implement a ticket
		// order: 'ticket_tail' is the next ticket, and 'ticket_head'
		// is the ticket currently being served.  You should set a local
		// variable to 'd->ticket_head' and increment 'd->ticket_head'.
		// Then, block at least until 'd->ticket_tail == local_ticket'.
		// (Some of these operations are in a critical section and must
		// be protected by a spinlock; which ones?)

		// Your code here (instead of the next two lines).
		//if file is already write locked, and we own it, this is deadlock so return
		//eprintk("ABOUT TO ACUIRE\n");
		osp_spin_lock(&(d->mutex));
		if ((d->isWriteLocked) && (d->writeLockPid == current->pid)){
			osp_spin_unlock(&(d->mutex));
			return -EDEADLK;
		}
		//eprintk("ACQUIREE 0000\n");

		unsigned ticket;
		ticket = d->ticket_tail++; //set own ticket number to ticket_tail, the next available ticket slot;
		//now add ticket into the array
		//if ticket_head is already == ticket,then we dont have traverse, set head to curr ticket
	/*	if (d->ticket_head == ticket){
			ticketListHead->ticketNum = ticket;
			ticketListHead->next = NULL;
		}else {*/
			struct ticketNode *listEnd = d->ticketListHead;
			while (listEnd->next != NULL){//traverse until listEnd points to the end of the ticketList
				listEnd = listEnd->next;
			}
			//here, listEnd points to the last ticket in the list, which should be blank and ready for use
			listEnd->ticketNum = ticket;
			struct ticketNode *newTicket = kmalloc (sizeof(struct ticketNode), GFP_ATOMIC); //create a new ticket for the next one to grab
			newTicket->ticketNum = -1; //create the new ticket
			newTicket->next = NULL;
			listEnd->next = newTicket; //add to the end of the queue
			newTicket->prev = listEnd;
	//	}
			//eprintk("ACQUIRE 1a \n");
		eprintk("ticket assigned: %d\n", ticket);
		//check if we already have a write lock, if we do we cannot request another lock
		//eprintk("ACQUIRE 1a \n");
		osp_spin_unlock(&(d->mutex));
		if (filp_writable){ //if file is open for writing
			//block until we're the next ticket to be executed, and we have free reign to get a lock
			eprintk("ACQUIRE 1wriiiite \n");
			if (wait_event_interruptible((d->blockq), (d->ticket_head == ticket) && (d->numReadLocks == 0) && (d->isWriteLocked == 0))){
				//signal checking. if we're here, wait was interrupted. give up lock
				osp_spin_lock(&(d->mutex));
				//remove our ticket from queue since a signal interrupted us
				if(listEnd != d->ticketListHead){
					listEnd->prev->next = listEnd->next;
					listEnd->next->prev = listEnd->prev;
				} else {
					d->ticketListHead = listEnd->next;
					listEnd->next->prev = NULL;
				}
				//eprintk("removing ticket: %d", listEnd->ticket
				kfree(listEnd);//drop the new ticket we made	
				if (d->ticketListHead->next == NULL){
				//if null, no new tickets to serve, we set head -> tail for immediate update
					d->ticket_head = d->ticket_tail;
				} else {
					d->ticket_head = d->ticketListHead->ticketNum;
				}
				osp_spin_unlock(&(d->mutex));
				return -ERESTARTSYS;
			}
			eprintk("ACQUIRE 22222\n");
			osp_spin_lock(&(d->mutex));
			//obtain the write lock, set it to true and put our pid down
			d->isWriteLocked = 1; //get the write lock
			d->writeLockPid = current->pid; //set process' pid to the writelock
			osp_spin_unlock(&(d->mutex));
		} else { //we want to grab a read lock instead
			//for read requests, we only care that there are no current writes. reads are okay
			eprintk("ACQUIRE 1reaaaad \n");
			if (wait_event_interruptible((d->blockq), (d->ticket_head == ticket) && (d->isWriteLocked == 0))){
				osp_spin_lock(&(d->mutex));
				//same as before, toss out our ticket if we get interrupted
				if(listEnd != d->ticketListHead){
					listEnd->prev->next = listEnd->next;
					listEnd->next->prev = listEnd->prev;
				} else {
					d->ticketListHead = listEnd->next;
					listEnd->next->prev = NULL;
				}
				kfree(listEnd);//drop the new ticket we made
				if (d->ticketListHead->next == NULL){
				//if null, no new tickets to serve, we set head -> tail for immediate update
					d->ticket_head = d->ticket_tail;
				} else {
					d->ticket_head = d->ticketListHead->ticketNum;
				}
				osp_spin_unlock(&(d->mutex));
				return -ERESTARTSYS;
			}
			//multiple readlocks can be held, so make sure that bit isnt already set
			//obtain a read lock, add ourselves to list of programs currently reading
			eprintk("ACQUIRE 2? \n");
			osp_spin_lock(&(d->mutex)); //add ticket to readerlist, and increment number of readers
			struct readerNode *readerListEnd = d->readerListHead;
			while (readerListEnd->next != NULL){//traverse until listEnd points to the end of the readerList
				readerListEnd = readerListEnd->next;
			}
			readerListEnd->pid = current->pid;
			eprintk("ACQUIRE 3? \n");
			struct readerNode *newTicket = kmalloc (sizeof(struct readerNode),GFP_ATOMIC); //create a new ticket for the next one to grab
			newTicket->pid = -1; //create the new ticket
			newTicket->next = NULL;
			readerListEnd->next = newTicket; //add t
			newTicket->prev = readerListEnd;
			d->numReadLocks++;
			eprintk("ACQUIRE 4? \n");
			osp_spin_unlock(&(d->mutex));
		}
		filp->f_flags |= F_OSPRD_LOCKED; //does this set regardless of state? ^= seems to be how you toggle
		//check if first ticket
		//toss out ticket now that we've been served
		osp_spin_lock(&(d->mutex));
		if(listEnd != d->ticketListHead){
			listEnd->prev->next = listEnd->next;
			listEnd->next->prev = listEnd->prev;
		} else {
			d->ticketListHead = listEnd->next;
			listEnd->next->prev = NULL;
		}
		eprintk("ACQUIRE 55555555555555\n");
		kfree(listEnd);//drop the new ticket we made
 		//set new ticketnum for the next "customer" to grab
		if (d->ticketListHead->next == NULL){
			//if null, no new tickets to serve, we set head -> tail for immediate update
			d->ticket_head = d->ticket_tail;
		} else {
			d->ticket_head = d->ticketListHead->ticketNum;
		}
		osp_spin_unlock(&(d->mutex));
		eprintk("ACQUIRE 66666666666666 \n");
		return 0;
	} else if (cmd == OSPRDIOCTRYACQUIRE) {

		// EXERCISE: ATTEMPT to lock the ramdisk.
		//
		// This is just like OSPRDIOCACQUIRE, except it should never
		// block.  If OSPRDIOCACQUIRE would block or return deadlock,
		// OSPRDIOCTRYACQUIRE should return -EBUSY.
		// Otherwise, if we can grant the lock request, return 0.

		// Your code here (instead of the next two lines).
		eprintk("TRYQACUIRE DEADLOCKS?\n");

		osp_spin_lock(&(d->mutex));
		if ((d->isWriteLocked) && (d->writeLockPid == current->pid)){
			osp_spin_unlock(&(d->mutex));
			return -EDEADLK;
		}
		//osp_spin_unlock(&(d->mutex));
		eprintk("TRYACQUIRE 0000\n");
		if (filp_writable){ //if file is open for writing
			//if there are read locks or already write locked, we return ebusy and dont care
			//osp_spin_lock(&(d->mutex));
			if ((d->numReadLocks != 0) || (d->isWriteLocked == 1)){
				osp_spin_unlock(&(d->mutex));
				return -EBUSY;
			}

			d->isWriteLocked = 1; //get the write lock
			d->writeLockPid = current->pid; //set process' pid to the writelock
		} else { //we want to grab a read lock instead
			//since multiple read locks are allowed, we only return busy if there is a write lock
			//osp_spin_lock(&(d->mutex));
			if ((d->isWriteLocked == 1)){
				osp_spin_unlock(&(d->mutex));
				return -EBUSY;
			}
			//multiple readlocks can be held, so make sure that bit isnt already set
			 //add ticket to readerlist, and increment number of readers
			struct readerNode *readerListEnd = d->readerListHead;
			while (readerListEnd->next != NULL){//traverse until listEnd points to the end of the readerList
				readerListEnd = readerListEnd->next;
			}
			readerListEnd->pid = current->pid;
			struct readerNode *newTicket = kmalloc (sizeof(struct readerNode),GFP_ATOMIC); //create a new ticket for the next one to grab
			newTicket->pid = -1; //create the new ticket
			newTicket->next = NULL;
			readerListEnd->next = newTicket; //add t
			newTicket->prev = readerListEnd;
			d->numReadLocks++;
		}
		osp_spin_unlock(&(d->mutex));
		eprintk("TRYACQUIRE PASSEDLOCKSTUFF\n");
		filp->f_flags |= F_OSPRD_LOCKED; //does this set regardless of state? ^= seems to be how you toggle
		return 0;

	} else if (cmd == OSPRDIOCRELEASE) {

		eprintk("BOUT TO REALEASE \n");
		// EXERCISE: Unlock the ramdisk.
		//
		// If the file hasn't locked the ramdisk, return -EINVAL.
		// Otherwise, clear the lock from filp->f_flags, wake up
		// the wait queue, perform any additional accounting steps
		// you need, and return 0.

		// Your code here (instead of the next line).

		//if trying to get rid of read
		//check if read lock exists, check with pid and readerNodes
		//
		osp_spin_lock(&(d->mutex));
		if(!(filp->f_flags & F_OSPRD_LOCKED)) //fiel has no lock
		{
			osp_spin_unlock(&(d->mutex));
			return -EINVAL;
		}
		eprintk("REALEASE 0000\n");

		if(filp_writable) //looking for write lock
		{
			if((d->isWriteLocked != 0) && (d->isWriteLocked != current->pid))//someoneone else has a lock on it
			{
				osp_spin_unlock(&(d->mutex)); //nothing to unlock if no write lock
				return -EINVAL;
			}
			d->isWriteLocked = 0; //unlock
			d->writeLockPid = -1;
			//osp_spin_unlock(&(d->mutex));
			//return 0;
		} else { //looking for read lock
			eprintk("GONNA READLOCK RELEASE 0000\n");
			if(d->numReadLocks==0)
			{
				osp_spin_unlock(&(d->mutex)); //nothing to unlock if no readlocks
				return -EINVAL;
			}
			int readLPidExists = 0;
			struct readerNode *traverse;
			traverse = d->readerListHead;
				//spot check if there's only one node
			while (traverse->next != NULL){//check if readlock with cur pid exists
				if(traverse->pid == current->pid){//if node has pid == current->pid, we need to remove from list of readers
				readLPidExists = 1;
				break;
				}
				traverse = traverse->next; //advance traverser
			}
			if (!readLPidExists)
			{
				osp_spin_unlock(&(d->mutex)); //nothing to unlock if no readlocks
				return -EINVAL;
			}

			traverse = d->readerListHead;
			//spot check if there's only one node
			while (traverse->next != NULL){
				if(traverse->pid == current->pid){//if node has pid == current->pid, we need to remove from list of readers
					struct readerNode *temp = traverse; //get a temp to point to current node so we can delete
					if(traverse != d->readerListHead){//if not first node
						traverse->prev->next = traverse->next;//re-link
						traverse->next->prev = traverse->prev;
					} else {
						d->readerListHead = traverse->next;//if is first node
						traverse->next->prev = NULL;
					}
					traverse = traverse->next; //advance traverser
					kfree(temp);
					d->numReadLocks--; //if we've tossed out a readerlist node, that means we have 1 fewer reader
				} else {
					traverse = traverse->next;
				}
			}
		}
		eprintk("REALEASE DONE\n");
		osp_spin_unlock(&d->mutex);
		filp->f_flags &= (~F_OSPRD_LOCKED);
		wake_up_all(&d->blockq);
		return 0;

	} else {
		r = -ENOTTY; /* unknown command */
		return r;
	}
}


// Initialize internal fields for an osprd_info_t.

static void osprd_setup(osprd_info_t *d)
{
	/* Initialize the wait queue. */
	init_waitqueue_head(&d->blockq);
	osp_spin_lock_init(&d->mutex);
	d->ticket_head = d->ticket_tail = 0;
	/* Add code here if you add fields to osprd_info_t. */
	d->numReadLocks = 0;
	d->isWriteLocked = 0;
	d->writeLockPid = -1;
	d->ticketListHead = (struct ticketNode *) kmalloc(sizeof(struct ticketNode),GFP_ATOMIC);
	d->ticketListHead->ticketNum = -1;
	d->ticketListHead->next = NULL;
	d->readerListHead = (struct readerNode*) kmalloc(sizeof(struct readerNode),GFP_ATOMIC);
	d->readerListHead->pid = -1;
	d->readerListHead->next = NULL;
}


/*****************************************************************************/
/*         THERE IS NO NEED TO UNDERSTAND ANY CODE BELOW THIS LINE!          */
/*                                                                           */
/*****************************************************************************/

// Process a list of requests for a osprd_info_t.
// Calls osprd_process_request for each element of the queue.

static void osprd_process_request_queue(request_queue_t *q)
{
	osprd_info_t *d = (osprd_info_t *) q->queuedata;
	struct request *req;

	while ((req = elv_next_request(q)) != NULL)
		osprd_process_request(d, req);
}


// Some particularly horrible stuff to get around some Linux issues:
// the Linux block device interface doesn't let a block device find out
// which file has been closed.  We need this information.

static struct file_operations osprd_blk_fops;
static int (*blkdev_release)(struct inode *, struct file *);

static int _osprd_release(struct inode *inode, struct file *filp)
{
	if (file2osprd(filp))
		osprd_close_last(inode, filp);
	return (*blkdev_release)(inode, filp);
}

static int _osprd_open(struct inode *inode, struct file *filp)
{
	if (!osprd_blk_fops.open) {
		memcpy(&osprd_blk_fops, filp->f_op, sizeof(osprd_blk_fops));
		blkdev_release = osprd_blk_fops.release;
		osprd_blk_fops.release = _osprd_release;
	}
	filp->f_op = &osprd_blk_fops;
	return osprd_open(inode, filp);
}


// The device operations structure.

static struct block_device_operations osprd_ops = {
	.owner = THIS_MODULE,
	.open = _osprd_open,
	// .release = osprd_release, // we must call our own release
	.ioctl = osprd_ioctl
};


// Given an open file, check whether that file corresponds to an OSP ramdisk.
// If so, return a pointer to the ramdisk's osprd_info_t.
// If not, return NULL.

static osprd_info_t *file2osprd(struct file *filp)
{
	if (filp) {
		struct inode *ino = filp->f_dentry->d_inode;
		if (ino->i_bdev
		    && ino->i_bdev->bd_disk
		    && ino->i_bdev->bd_disk->major == OSPRD_MAJOR
		    && ino->i_bdev->bd_disk->fops == &osprd_ops)
			return (osprd_info_t *) ino->i_bdev->bd_disk->private_data;
	}
	return NULL;
}


// Call the function 'callback' with data 'user_data' for each of 'task's
// open files.

static void for_each_open_file(struct task_struct *task,
		  void (*callback)(struct file *filp, osprd_info_t *user_data),
		  osprd_info_t *user_data)
{
	int fd;
	task_lock(task);
	spin_lock(&task->files->file_lock);
	{
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 13)
		struct files_struct *f = task->files;
#else
		struct fdtable *f = task->files->fdt;
#endif
		for (fd = 0; fd < f->max_fds; fd++)
			if (f->fd[fd])
				(*callback)(f->fd[fd], user_data);
	}
	spin_unlock(&task->files->file_lock);
	task_unlock(task);
}


// Destroy a osprd_info_t.

static void cleanup_device(osprd_info_t *d)
{
	wake_up_all(&d->blockq);
	if (d->gd) {
		del_gendisk(d->gd);
		put_disk(d->gd);
	}
	if (d->queue)
		blk_cleanup_queue(d->queue);
	if (d->data)
		vfree(d->data);
}


// Initialize a osprd_info_t.

static int setup_device(osprd_info_t *d, int which)
{
	memset(d, 0, sizeof(osprd_info_t));

	/* Get memory to store the actual block data. */
	if (!(d->data = vmalloc(nsectors * SECTOR_SIZE)))
		return -1;
	memset(d->data, 0, nsectors * SECTOR_SIZE);

	/* Set up the I/O queue. */
	spin_lock_init(&d->qlock);
	if (!(d->queue = blk_init_queue(osprd_process_request_queue, &d->qlock)))
		return -1;
	blk_queue_hardsect_size(d->queue, SECTOR_SIZE);
	d->queue->queuedata = d;

	/* The gendisk structure. */
	if (!(d->gd = alloc_disk(1)))
		return -1;
	d->gd->major = OSPRD_MAJOR;
	d->gd->first_minor = which;
	d->gd->fops = &osprd_ops;
	d->gd->queue = d->queue;
	d->gd->private_data = d;
	snprintf(d->gd->disk_name, 32, "osprd%c", which + 'a');
	set_capacity(d->gd, nsectors);
	add_disk(d->gd);

	/* Call the setup function. */
	osprd_setup(d);

	return 0;
}

static void osprd_exit(void);


// The kernel calls this function when the module is loaded.
// It initializes the 4 osprd block devices.

static int __init osprd_init(void)
{
	int i, r;

	// shut up the compiler
	(void) for_each_open_file;
#ifndef osp_spin_lock
	(void) osp_spin_lock;
	(void) osp_spin_unlock;
#endif

	/* Register the block device name. */
	if (register_blkdev(OSPRD_MAJOR, "osprd") < 0) {
		printk(KERN_WARNING "osprd: unable to get major number\n");
		return -EBUSY;
	}

	/* Initialize the device structures. */
	for (i = r = 0; i < NOSPRD; i++)
		if (setup_device(&osprds[i], i) < 0)
			r = -EINVAL;

	if (r < 0) {
		printk(KERN_EMERG "osprd: can't set up device structures\n");
		osprd_exit();
		return -EBUSY;
	} else
		return 0;
}


// The kernel calls this function to unload the osprd module.
// It destroys the osprd devices.

static void osprd_exit(void)
{
	int i;
	for (i = 0; i < NOSPRD; i++)
		cleanup_device(&osprds[i]);
	unregister_blkdev(OSPRD_MAJOR, "osprd");
}


// Tell Linux to call those functions at init and exit time.
module_init(osprd_init);
module_exit(osprd_exit);
