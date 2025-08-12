#include <stdio.h>     
#include <stdlib.h>     
#include <string.h>    
#include <unistd.h>     
#include <fcntl.h>      
#include <sys/mman.h>   
#include <sys/stat.h>  
#include <semaphore.h>  

#define SHM_NAME "/shared_memory" 
#define SEM_PARENT "/sem_parent"   
#define MAX_CHILDREN 100          
#define MESSAGE_SIZE 256        

typedef struct {
    char message[MESSAGE_SIZE];// buffer
    int child_pids[MAX_CHILDREN];  // pinakas me child PIDs
    int child_count;  // counter gia to posa paidia exw ftiaksei
    int child_start_steps[MAX_CHILDREN]; // start time step
} SharedMemory;


int main() {
    // unlink apo prohgoumenh ektelesh
    shm_unlink(SHM_NAME);     
    sem_unlink(SEM_PARENT);   

    // read write permissions shared mem
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666); 
    if (shm_fd == -1) {       
        perror("shm_open failed");
        exit(1);
    }

    //sharedmemobejct=sharedmem
    if (ftruncate(shm_fd, sizeof(SharedMemory)) == -1) {
        perror("ftruncate failed");
        exit(1);
    }

    // map thn shared memory sto process
    SharedMemory *shm_ptr = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE,
                                 MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {  
        perror("mmap failed");
        exit(1);
    }

    // init to shared mem me 0
    memset(shm_ptr, 0, sizeof(SharedMemory));
    shm_ptr->child_count = 0;    //  den exoume paidia akoma

    //  semaphore gia ton parent me timh 1
    sem_t *sem_parent = sem_open(SEM_PARENT, O_CREAT | O_EXCL, 0666, 1);
    if (sem_parent == SEM_FAILED) {             
        perror("sem_open (parent) failed");
        exit(1);
    }

    // debug
    printf("Semaphore /sem_parent created successfully.\n");
    printf("Shared memory created at: %p\n", (void *)shm_ptr);
    printf("Shared memory and semaphores successfully initialized.\n");
 //cleanum
    munmap(shm_ptr, sizeof(SharedMemory));
    sem_close(sem_parent);

    return 0; 
}
