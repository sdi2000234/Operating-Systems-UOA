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


int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Child usage: %s <child_index>\n", argv[0]);
        exit(1);
    }
    
    int child_index = atoi(argv[1]);
    if (child_index < 0 || child_index >= MAX_CHILDREN) {
        // elegxos oti einai mesa sto range
        fprintf(stderr, "Invalid child index %d\n", child_index);
        exit(1);
    }

    // open to shared mem object pou anoikse prin o parent
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open failed in child");
        exit(1);
    }

    // map gia to shared mem
    SharedMemory *shm_ptr = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        perror("mmap failed in child");
        exit(1);
    }

    // open ton parent semaphore
    sem_t *sem_parent = sem_open(SEM_PARENT, 0);
    if (sem_parent == SEM_FAILED) {
        perror("sem_open parent failed in child");
        exit(1);
    }

    // ftiakse ton semaphore(onoma)
    char sem_child_name[64];
    snprintf(sem_child_name, sizeof(sem_child_name), "/sem_child_%d", child_index);
    sem_t *sem_child = sem_open(sem_child_name, 0);
    if (sem_child == SEM_FAILED) {
        perror("sem_open child failed in child");
        exit(1);
    }

    int messages_processed = 0;  // counter gia ta mhnmata pou ekane process
    int start_step = shm_ptr->child_start_steps[child_index]; // start step apo thn shared memory

    while (1) {
        sem_wait(sem_child); // perimenw mexri na kanei post o parent me neo munhma

        char buffer[MESSAGE_SIZE];  // Local buffer, krataw antigrafo tou mhnhmatos pou phra
        strncpy(buffer, shm_ptr->message, MESSAGE_SIZE); // copy to mhnyma tou shared memory ston buffer
        buffer[MESSAGE_SIZE-1] = '\0'; // null sto telos

        // elegxos an phra terminate
        if (strncmp(buffer, "TERMINATE:", 10) == 0) {
            int end_step = atoi(buffer + 10);   // end_step 
            int total_active_steps = end_step - start_step; // posa steps htan active
            printf("Child PID %d processed %d messages and was active for %d steps before termination.\n",
                   getpid(), messages_processed, total_active_steps);
            
            sem_post(sem_parent); // ACK oti elava to TERMINATE, steilto stonparent
            break;  //  break gia na kanw terminate
        } else {
            //  kanoniko mhnuma, auksanw to counter twn mhnymatwn pou ekane process
            messages_processed++;
            sem_post(sem_parent); // ACK ston parent oti perase to mhnyma
        }
    }

    // unmap shared memory kai kleinw ta semaphores
    munmap(shm_ptr, sizeof(SharedMemory));
    sem_close(sem_parent);
    sem_close(sem_child);

    return 0;
}
