#include <stdio.h>              
#include <stdlib.h> 
#include <string.h>             
#include <unistd.h>             
#include <fcntl.h>              
#include <sys/mman.h>          
#include <sys/stat.h>            
#include <semaphore.h>           
#include <sys/types.h>           
#include <sys/wait.h>            
#include <time.h>              

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

static sem_t *sem_parent = NULL;          // Global pointer -> parent semaphore
static SharedMemory *shm_ptr = NULL;      // Global pointer-> shared memory structure
static sem_t* child_sems[MAX_CHILDREN];  // Array apo semaphores gia kathe  child

static void create_child_sem(int idx) {
    char sem_child_name[64];   // Buffer gia to onoma tou semaphore
    snprintf(sem_child_name, sizeof(sem_child_name), "/sem_child_%d", idx); // monadiko onoma gia kathe semaphore
    sem_unlink(sem_child_name);  // unlink
    child_sems[idx] = sem_open(sem_child_name, O_CREAT | O_EXCL, 0666, 0); // arxikopoiw me 0
    if (child_sems[idx] == SEM_FAILED) { 
        perror("sem_open child failed in parent"); //fail
        exit(1);
    }
}

static void spawn_child(int child_index, int current_step) { // spawn child gia sigkekrimeno index, an den trexei hdh
    if (shm_ptr->child_pids[child_index] != 0) {
        return; // an uparxei PID, tote trexei hdh, den kanw kati
    }
    create_child_sem(child_index);
    pid_t pid = fork();
    if (pid == 0) {  // path tou Child process 
        char idx_str[10]; // buffer gia to index tou child
        snprintf(idx_str, sizeof(idx_str), "%d", child_index); // kanw to index string
        execl("./child", "child", idx_str, (char*)NULL);  // antikathistw to child image me to executable
        perror("execl failed");    // fail
        exit(1);
    } else if (pid > 0) {  // path meta to fork
        shm_ptr->child_pids[child_index] = pid;  // apothikefsi tou PID
        if (child_index+1 > shm_ptr->child_count) {
            shm_ptr->child_count = child_index+1;
        }
        shm_ptr->child_start_steps[child_index] = current_step; // katagrafh tou start step tou sugkekrimenou paidiou
    } else {
        perror("fork failed");// fail
    }
}

static void send_message_to_child(int child_index, const char* msg, int is_terminate, int end_step) {
    if (shm_ptr->child_pids[child_index] == 0) return;   // an den uparxei child se auto to index den kanw tpt

    sem_wait(sem_parent); // perimenw to sem parent gia na trexoun tautoxrona
    if (is_terminate) {
        char buffer[MESSAGE_SIZE];
        snprintf(buffer, sizeof(buffer), "TERMINATE:%d", end_step);
        strncpy(shm_ptr->message, buffer, MESSAGE_SIZE); // mhnuma gia to termination, to vazw kai auto sto shared mem
    } else {
        strncpy(shm_ptr->message, msg, MESSAGE_SIZE); // vazw to minima sto shared memory
    }
    shm_ptr->message[MESSAGE_SIZE - 1] = '\0';// gia na eimai siguros oti termatise

    sem_post(child_sems[child_index]);  // Signal sto child semaphore gia na diavasei to minima

    if (!is_terminate) {
        // an einai kanoniko mhnima, perimene gia to ACK tou chil If normal message, wait for child's acknowledgment
        sem_wait(sem_parent);
        // release sem_parent afou parw to ACK gia na gurisw sto arxiko state
        sem_post(sem_parent);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 4) { // cmnd line args
        fprintf(stderr, "Usage: %s <M> <K> <command_file>\n", argv[0]);
        exit(1);
    }
    int M = atoi(argv[1]);
    int K = atoi(argv[2]);
    char *command_file_name = argv[3]; // Command file name

    if (K > MAX_CHILDREN) { // elegxos gia to K
        fprintf(stderr, "K exceeds MAX_CHILDREN limit.\n");
        exit(1);
    }

    if (M < K+1) {   // PREPEI na isxyei oti M einai toulaxiston ison me K+1
        fprintf(stderr, "M must be at least K+1 (M=%d, K=%d)\n", M, K);
        exit(1);
    }

    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666); // anoigw to shared memory
    if (shm_fd == -1) {
        perror("shm_open failed in parent");
        exit(1);
    }

    // map me to shared memory
    shm_ptr = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        perror("mmap failed in parent");
        exit(1);
    }

    // Open the parent semaphore (should have been created by sharedmem)
    sem_parent = sem_open(SEM_PARENT, 0);
    if (sem_parent == SEM_FAILED) {
        perror("sem_open parent failed in parent");
        exit(1);
    }

    // pinakas me shmaioforoi paidiwn
    for (int i = 0; i < K; i++) {
        child_sems[i] = NULL;
    }

    // arxeio config
    FILE *command_file = fopen(command_file_name, "r");
    if (!command_file) {
        perror("Failed to open command file");
        exit(1);
    }

    // arxeio me keimeno
    FILE *message_file = fopen("mobydick.txt", "r");
    if (!message_file) {
        perror("Failed to open mobydick.txt");
        exit(1);
    }
    srand(time(NULL));  // epilogh tuxaiou paidiou
    char line[256];     // buffer
    int running = 1;    // metavliti flag gia na kserw an trexw akoma h oxi 
    int current_step = 0; // timestampo apo ta commands
    // diavazw ews otou teleiwsoun oi grammes h ean lavw mhnyma na stamatisw 
    while (running && fgets(line, sizeof(line), command_file)) {
        int timestamp;
        char process_label[32], command[32];
        int n = sscanf(line, "%d %s %s", &timestamp, process_label, command); // parse grammwn

        if (n == 2 && strcmp(process_label, "EXIT") == 0) {
            //  an phra "timestamp EXIT"
            current_step = timestamp;
            //terminate gia ola ta paidia procesces
            for (int i = 0; i < shm_ptr->child_count; i++) {
                if (shm_ptr->child_pids[i] != 0) {
                    send_message_to_child(i, NULL, 1, current_step);  //TERMINATE
                    waitpid(shm_ptr->child_pids[i], NULL, 0); // perimenw na kanei exit
                    shm_ptr->child_pids[i] = 0; // markarw to paidi san terminated
                }
            }
            running = 0; // afou phra exit, stop
        } else if (n == 3) {
            current_step = timestamp;
            int child_index = -1;
            if (process_label[0] == 'C') { // an ksekinaei me C, einai child 
                child_index = atoi(&process_label[1]) - 1; // metatrepw "C<number>" se index(ksekinontas apo to 0)
                if (child_index < 0 || child_index >= K) {
                    fprintf(stderr, " Invalid child index %d\n", child_index);
                    continue;
                }
            }
            if (command[0] == 'S') {
                // S: SPAWN ena neo chiild, an den to exw kanei hdh
                spawn_child(child_index, current_step);
            } else if (command[0] == 'T') {
                // T: TERMINATE  ena sugkekrikmeno child
                if (shm_ptr->child_pids[child_index] != 0) {
                    send_message_to_child(child_index, NULL, 1, current_step); // terminate chilld
                    waitpid(shm_ptr->child_pids[child_index], NULL, 0);  // perimenw to paidi na termatisei
                    shm_ptr->child_pids[child_index] = 0; // markarw san terminated
                }
            } else if (strcmp(command, "EXIT") == 0) {
                // exit, ara termatizw ola ta paidia
                for (int i = 0; i < shm_ptr->child_count; i++) {
                    if (shm_ptr->child_pids[i] != 0) {
                        send_message_to_child(i, NULL, 1, current_step);
                        waitpid(shm_ptr->child_pids[i], NULL, 0);
                        shm_ptr->child_pids[i] = 0;
                    }
                }
                running = 0; // stamataw
            }
        } else {
            // an to mhnyma pou phra den einai sto format tou mhnymatos pou perimenw (px 21 C3 S), elegxw an einai exit
            int timestamp2;
            char cmd_only[32];
            if (sscanf(line, "%d %s", &timestamp2, cmd_only) == 2 && strcmp(cmd_only, "EXIT") == 0) {
                // an parw exit
                current_step = timestamp2;
                // termatizw ola ta paidia
                for (int i = 0; i < shm_ptr->child_count; i++) {
                    if (shm_ptr->child_pids[i] != 0) {
                        send_message_to_child(i, NULL, 1, current_step);
                        waitpid(shm_ptr->child_pids[i], NULL, 0);
                        shm_ptr->child_pids[i] = 0;
                    }
                }
                running = 0;
            }
        }

        // an trexei akoma kai yparxoun energa paidia, stile mia grmmh me ena mhnyma se ena tuxaio paidi 
        if (running) {
            int active_indices[MAX_CHILDREN];
            int active_count = 0;
            for (int i = 0; i < shm_ptr->child_count; i++) {
                if (shm_ptr->child_pids[i] != 0) //an einai energo
                    active_indices[active_count++] = i;
            }

            if (active_count > 0) {
                // dialekse ena tuxaio energo paidi
                int target_child = active_indices[rand() % active_count];

                char message_buffer[MESSAGE_SIZE];
                if (fgets(message_buffer, sizeof(message_buffer), message_file) == NULL) {
                    rewind(message_file); // If we reach EOF, rewind the file
                    fgets(message_buffer, sizeof(message_buffer), message_file); // diavase thn prwth grammh
                }

                size_t len = strlen(message_buffer);
                if (len > 0 && message_buffer[len-1] == '\n') {
                    message_buffer[len-1] = '\0'; // an petuxw newline , thn afairw
                }

                send_message_to_child(target_child, message_buffer, 0, 0); // kanoniko minhma
            }
        }
    }

    fclose(command_file);  // kleinw to command file
    fclose(message_file);  // kleinw to message file

    munmap(shm_ptr, sizeof(SharedMemory)); // cleanup
    sem_close(sem_parent);
    sem_unlink(SEM_PARENT);
    shm_unlink(SHM_NAME);  // afairw to shared memory object
    for (int i = 0; i < K; i++) {
        if (child_sems[i]) {
            char sem_child_name[64];
            snprintf(sem_child_name, sizeof(sem_child_name), "/sem_child_%d", i);
            sem_close(child_sems[i]);
            sem_unlink(sem_child_name);
        }
    }

    return 0;
}
