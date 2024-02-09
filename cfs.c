#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#define PROCESS_FILE "processes_to_run.csv"		// the file that contains preconfigured tasks to execute
#define NUM_CONSUMER_THREADS 4					// AKA number of "CPUs" for the sake of simulation
#define MAX_TASKS 256							// maximum tasks a single queue can hold
#define NUM_QUEUES 3							// RQ0, RQ1, and RQ2
#define MAX_SLEEP_AVG 10						// 10 ms is max sleep average
#define INVALID_PID 0							// pid 0 is invalid, so we use this to mark empty slots in queue
#define ANY_CPU -1								// the cpu affinity for "any cpu"
#define RQ0 0									// index of RQ0
#define RQ1 1									// index of RQ1
#define RQ2 2									// index of RQ2

// macros for calculating dynamic priority
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// scheduling policies
typedef enum {
	FIFO,	// run task to completion
	RR,		// run task until timeslice is done
	NORMAL,
} sched_policy;

// structure to hold task data
typedef struct {
	int pid; 
	int static_priority; 
	int previous_priority;
	int dynamic_priority; 
	int remain_time;	// total execution time to complete a task
	int exec_time;	// execution time for this iteration
	int time_slice;
	int accu_time_slice;
	int last_cpu;
	int cpu_affinity;
	sched_policy policy;
	int blocked;	// for NORMAL tasks
	struct timeval blocked_start;
	long int blocked_time;	// duration that it spent blocked
	int sleep_avg;	
} task_t;

task_t tasks[NUM_QUEUES][MAX_TASKS];	// three queues (RQ0, RQ1, RQ2) of max tasks
pthread_mutex_t mutex;					// for locking the ready queue

// function declarations
sched_policy get_scheduling_policy(const char *policy_str);
int parse_csv_line(FILE *file, task_t *task_data);
void *thread_function_p(void *arg);
void *thread_function_c(void *arg);
void add_to_ready_queue(task_t task); 
int extract_from_ready_queue(int cpu_id, task_t *task_to_extract);
const char* get_policy_string(sched_policy policy); 

/*
 * Main function to start the CFS simulation.
 */
int main() {
    int res;
    int thread_num;
	pthread_t producer;
	pthread_t consumers[NUM_CONSUMER_THREADS];
	void *thread_result;
	
	// init mutex used for ready queue access
	res = pthread_mutex_init(&mutex, NULL);
	if (res != 0) {
		perror("mutex initialization failed");
		exit(EXIT_FAILURE);
	}	

  	// create producer thread 
    res = pthread_create(&(producer), NULL, thread_function_p, NULL);
    if (res != 0) {
		perror("Thread creation failed");
		exit(EXIT_FAILURE);
    }

	// join producer thread because we have to let producer finish creating tasks first
	printf("Waiting for producer thread to finish...\n");
    res = pthread_join(producer, &thread_result);
	printf("producer thread has joined back\n");
    if (res != 0) {
        perror("Thread join failed");
        exit(EXIT_FAILURE);
    }

	// create consumer threads (simulating CPUs)
    for (thread_num = 0; thread_num < NUM_CONSUMER_THREADS; thread_num++) {
        res = pthread_create(&(consumers[thread_num]), NULL, thread_function_c, (void *)&thread_num);
        if (res != 0) {
			perror("Thread creation failed");
			exit(EXIT_FAILURE);
        }
        sleep(1);
    }
    printf("Waiting for consumer threads to finish...\n");

    // call pthread_join () here with a loop
    for (thread_num = 0; thread_num < NUM_CONSUMER_THREADS; thread_num++) {
		res = pthread_join(consumers[thread_num], &thread_result);
		if (res != 0) {
			perror("Thread join failed");
			exit(EXIT_FAILURE);
		}
	}
    
    printf("All done\n");

    exit(EXIT_SUCCESS);
}

/*
 * Function run by consumer threads (simultating CPUs) to extract a task
 * from a ready queue, execute the task, then put back into the queue if
 * the task is incomplete after its time slice.
 */
void *thread_function_c(void *arg) {  
    int my_cpu_number = *(int *)arg;
    task_t generated_info;

	while (1) {
		if (extract_from_ready_queue(my_cpu_number, &generated_info)) {
			printf("\nConsumer %d recieved: \n", my_cpu_number);

			printf("pid: %d, \n", generated_info.pid);
			printf("SP: %d, \n", generated_info.static_priority);
			printf("DP: %d, \n", generated_info.dynamic_priority);
			printf("previous_priority: %d, \n", generated_info.previous_priority);
			printf("remain_time: %d, \n", generated_info.remain_time);
			printf("time_slice: %d, \n", generated_info.time_slice);
			printf("accu_time_slice: %d, \n", generated_info.accu_time_slice);
			printf("last_cpu: %d, \n", generated_info.last_cpu);
			printf("cpu_affinity: %d, \n", generated_info.cpu_affinity);
			printf("policy: %s, \n", get_policy_string(generated_info.policy));

			// Calculate time slice
			if (generated_info.policy != FIFO) {
				if (generated_info.static_priority < 120) {
					generated_info.time_slice = ((140 - generated_info.static_priority) * 20);
				} else {
					generated_info.time_slice = ((140 - generated_info.static_priority) * 5);
				}
			} else {
				generated_info.time_slice = generated_info.remain_time;	// FIFO executes the entire remain time
			}

			// Decrease remaining time and increase accumulated time slice
			switch (generated_info.policy) {
				case FIFO:
					usleep(generated_info.remain_time * 1000);	// times 1000 for ms
					generated_info.exec_time = generated_info.time_slice;
					generated_info.remain_time -= generated_info.time_slice; // decrease time slice
					generated_info.accu_time_slice += generated_info.time_slice;  // increase accumulated time slice
					break;
				case RR:
					usleep(generated_info.time_slice * 1000);	// times 1000 for ms
					generated_info.exec_time = generated_info.time_slice;
					generated_info.remain_time -= generated_info.time_slice; // decrease time slice
					generated_info.accu_time_slice += generated_info.time_slice;  // increase accumulated time slice
					break;
				case NORMAL:
					// randomly generate how long the task actually spent executing in this time slice
					srand(time(NULL));
					generated_info.exec_time = (rand() % 10 + 1) * 10;	 
					// make sure you don't run longer than time slice
					if (generated_info.exec_time > generated_info.time_slice) {
						generated_info.exec_time = generated_info.time_slice;
					}

					usleep(generated_info.exec_time * 1000);	// times 1000 for ms

					// check for left over time to see if we need to block this task
					if (generated_info.exec_time < generated_info.time_slice) {
						generated_info.blocked = 1;	
						// mark the time this task was blocked
						gettimeofday(&generated_info.blocked_start, NULL);
					} 

					// decrease time slice
					generated_info.remain_time -= generated_info.exec_time;	 
					generated_info.accu_time_slice += generated_info.exec_time;  

					// adjust priority (only for NORMAL tasks)
					generated_info.previous_priority = generated_info.dynamic_priority;
					generated_info.dynamic_priority = MAX(100, MIN((generated_info.dynamic_priority - generated_info.sleep_avg + 5), 139));
					break;
			}

			if (generated_info.remain_time < 0) {
				generated_info.remain_time = 0;
			}

			// set last cpu to this thread/cpu number
			generated_info.last_cpu = my_cpu_number;
		   
			printf("\nConsumer %d returned: \n", my_cpu_number);
			
			printf("pid: %d, \n", generated_info.pid);
			printf("SP: %d, \n", generated_info.static_priority);
			printf("DP: %d, \n", generated_info.dynamic_priority);
			printf("previous_priority: %d, \n", generated_info.previous_priority);
			printf("remain_time: %d, \n", generated_info.remain_time);
			printf("time_slice: %d, \n", generated_info.time_slice);
			printf("execution time for this iteration: %d, \n", generated_info.exec_time);
			printf("accu_time_slice: %d, \n", generated_info.accu_time_slice);
			printf("last_cpu: %d, \n", generated_info.last_cpu);
			printf("cpu_affinity: %d, \n", generated_info.cpu_affinity);
			printf("policy: %s, \n", get_policy_string(generated_info.policy));

			usleep(generated_info.remain_time/1000);

			if (generated_info.remain_time > 0) {
				// task not done executing yet so put it back into the queue
				add_to_ready_queue(generated_info);	
			}
		} else {
			continue;	// nothing to run
		}
	}
    pthread_exit(NULL);
}

/*
 * Function run by producer thread that reads and parses a csv file and creates 
 * tasks (simulating user processes) and puts them into a ready queue for the 
 * consumers (simulating CPUs) to "execute".
 */
void *thread_function_p(void *arg) {
	printf("This is the producer\n");

	FILE *file = fopen(PROCESS_FILE, "r");	
	if (file == NULL) {
		perror("error on opening file");
		exit(EXIT_FAILURE);
	}
   
  	// generate tasks and put them into the ready queue	
	task_t task_data;

	while (1) {
		int result = parse_csv_line(file, &task_data);

        if (result == 1) {
            // Successfully parsed a line, task_data has been initialized, so add to queue
			printf("adding process to a ready queue\n");
			add_to_ready_queue(task_data); 
        } else if (result == 0) {
            // End of file or invalid input line
			printf("end of file or invalid input line\n");
            break;
        } else {
            // Parsing error
            fprintf(stderr, "Error parsing CSV line\n");
            break;
        }
	}
    pthread_exit(NULL);
}

/*
 * Adds a task pointed to by task_data to a ready queue based on its static priority.
 */ 
void add_to_ready_queue(task_t task_data) {
	int rq;	// index of the ready queue to put the task in
	pthread_mutex_lock(&mutex);	// acquire mutex because ready queue is shared

	// check which ready queue the task should be put in
	if (task_data.dynamic_priority >= 0 && task_data.dynamic_priority < 100) {
		rq = RQ0;
	} else if (task_data.dynamic_priority >= 100 && task_data.dynamic_priority < 140) {
		rq = RQ1;
	} else {
		rq = RQ2;
	}

	for (int idx = 0; idx < MAX_TASKS; idx++) {
		if (tasks[rq][idx].pid == INVALID_PID) {
			tasks[rq][idx] = task_data;	
			pthread_mutex_unlock(&mutex);	// done with ready queue, release mutex 
			return;
		}	
	}
}

/*
 * Extracts a task from the ready queue whose cpu affinity includes cpu_id.
 */
int extract_from_ready_queue(int cpu_id, task_t *task_to_extract) {
	pthread_mutex_lock(&mutex);	// acquire mutex because ready queue is shared
	int idx_of_best_task = -1;
	int rq_of_best_task = -1;
	int priority_of_best_task = 140;	// cannot be lower prio than this 
	int idx;

	// find the task with the max priority ignoring ones that can't be run on this cpu
	for (int rq = RQ0; rq < RQ2; rq++) {
		for (idx = 0; idx < MAX_TASKS; idx++) {
			if (tasks[rq][idx].pid == INVALID_PID) {	// empty slot in the queue
				continue;	// slot is empty, ignore it
			} else if (!(tasks[rq][idx].cpu_affinity == ANY_CPU || tasks[rq][idx].cpu_affinity == cpu_id)) {
				continue;	// task can only be run on another cpu, ignore it
			} 

			// we have a task that can be run on this cpu
			if (priority_of_best_task > tasks[rq][idx].dynamic_priority) {
				// and that task is a higher priority than any others we've seen so far
				idx_of_best_task = idx;
				rq_of_best_task = rq;
				priority_of_best_task = tasks[rq][idx].dynamic_priority;	
			}
		}
	}

	// done looking through all 3 queues, check if we got anything
	if (idx_of_best_task == -1) {
		// there are no tasks in any RQ that can be run on this cpu
		pthread_mutex_unlock(&mutex);	// done with ready queue, release mutex 
		return 0;	// no task
	} else {
		// got something to run
		*task_to_extract = tasks[rq_of_best_task][idx_of_best_task];

		if (task_to_extract->policy == NORMAL && task_to_extract->blocked) {
			// mark the task as unblocked and then measure the time it was blocked for
			task_to_extract->blocked = 0;	
			struct timeval end;
			gettimeofday(&end, NULL);
			task_to_extract->blocked_time = (end.tv_sec * 1000 + end.tv_usec / 1000) - 
							(task_to_extract->blocked_start.tv_sec * 1000 +
							task_to_extract->blocked_start.tv_usec / 1000);

			// adjust sleep_avg because the process is leaving blocked state (from Tadgh's announcement)
			task_to_extract->sleep_avg += task_to_extract->blocked_time / task_to_extract->time_slice - 1;
			if(task_to_extract->sleep_avg < 0) {
				 task_to_extract->sleep_avg = 0;
			} else if (task_to_extract->sleep_avg > MAX_SLEEP_AVG) {
				 task_to_extract->sleep_avg = MAX_SLEEP_AVG;
			}
		}
		tasks[rq_of_best_task][idx_of_best_task].pid = INVALID_PID;	// empty this slot
		pthread_mutex_unlock(&mutex);	// done with ready queue, release mutex 
		return 1;	// have a task
	}
}

/*
 * Function to convert a string to the corresponding sched_policy enum value.
 */
sched_policy get_scheduling_policy(const char *policy_str) {
    if (strcmp(policy_str, "NORMAL") == 0) {
        return NORMAL;
    } else if (strcmp(policy_str, "RR") == 0) {
        return RR;
    } else if (strcmp(policy_str, "FIFO") == 0) {
        return FIFO;
    } else {
        // Default to NORMAL if the string doesn't match any known type
        return NORMAL;
    }
}

/*
 * Function to get the scheduling policy string from the enum value.
 */
const char* get_policy_string(sched_policy policy) {
    switch (policy) {
        case FIFO:
            return "FIFO";
        case RR:
            return "RR";
        case NORMAL:
            return "NORMAL";
    }
}

/*
 * Function to parse a CSV line and store the data in the task_t struct.
 */
int parse_csv_line(FILE *file, task_t *task_data) {
    char policy_str[7]; // Temporary buffer for the policy string

    int result = fscanf(file, "%d,%6[^,],%d,%d,%d",
                        &task_data->pid,
                        policy_str,
                        &task_data->static_priority,
                        &task_data->remain_time,
                        &task_data->cpu_affinity);

    if (result == EOF) {
        // fscanf returns 0 when it fails to match any items
        return 0;
    } else if (result != 5) {
        // Parsing error
        return -1;
    }

    // Convert the policy string to the corresponding enum value
    task_data->policy = get_scheduling_policy(policy_str);

	// we use dynamic priority to figure out what task to extract, but when dynamic
	// priority isn't used or hasn't been initialized yet, use static priority
	task_data->dynamic_priority = task_data->static_priority;

	// no previous priority yet so just set it to what the dynamic priority is
	task_data->previous_priority = task_data->dynamic_priority;

    return 1; // Success
}

