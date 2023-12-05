// 
// tsh - A tiny shell program with job control
// 
// Ryan Cross
// rycr3278

using namespace std;

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <string>

#include "globals.h"
#include "jobs.h"
#include "helper-routines.h"

//
// Needed global variable definitions
//

static char prompt[] = "tsh> ";
int verbose = 0;

//
// You need to implement the functions eval, builtin_cmd, do_bgfg,
// waitfg, sigchld_handler, sigstp_handler, sigint_handler
//
// The code below provides the "prototypes" for those functions
// so that earlier code can refer to them. You need to fill in the
// function bodies below.
// 

void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

//
// main - The shell's main routine 
//
int main(int argc, char **argv) 
{
  int emit_prompt = 1; // emit prompt (default)

  //
  // Redirect stderr to stdout (so that driver will get all output
  // on the pipe connected to stdout)
  //
  dup2(1, 2);

  /* Parse the command line */
  char c;
  while ((c = getopt(argc, argv, "hvp")) != EOF) {
    switch (c) {
    case 'h':             // print help message
      usage();
      break;
    case 'v':             // emit additional diagnostic info
      verbose = 1;
      break;
    case 'p':             // don't print a prompt
      emit_prompt = 0;  // handy for automatic testing
      break;
    default:
      usage();
    }
  }

  //
  // Install the signal handlers
  //

  //
  // These are the ones you will need to implement
  //
  Signal(SIGINT,  sigint_handler);   // ctrl-c
  Signal(SIGTSTP, sigtstp_handler);  // ctrl-z
  Signal(SIGCHLD, sigchld_handler);  // Terminated or stopped child

  //
  // This one provides a clean way to kill the shell
  //
  Signal(SIGQUIT, sigquit_handler); 

  //
  // Initialize the job list
  //
  initjobs(jobs);

  //
  // Execute the shell's read/eval loop
  //
  for(;;) {
    //
    // Read command line
    //
    if (emit_prompt) {
      printf("%s", prompt);
      fflush(stdout);
    }

    char cmdline[MAXLINE];

    if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin)) {
      app_error("fgets error");
    }
    //
    // End of file? (did user type ctrl-d?)
    //
    if (feof(stdin)) {
      fflush(stdout);
      exit(0);
    }

    //
    // Evaluate command line
    //
    eval(cmdline);
    fflush(stdout);
    fflush(stdout);
  } 

  exit(0); //control never reaches here
}
  
/////////////////////////////////////////////////////////////////////////////
//
// eval - Evaluate the command line that the user has just typed in
// 
// If the user has requested a built-in command (quit, jobs, bg or fg)
// then execute it immediately. Otherwise, fork a child process and
// run the job in the context of the child. If the job is running in
// the foreground, wait for it to terminate and then return.  Note:
// each child process must have a unique process group ID so that our
// background children don't receive SIGINT (SIGTSTP) from the kernel
// when we type ctrl-c (ctrl-z) at the keyboard.
//
void eval(char *cmdline) 
{
    char *argv[MAXARGS]; // Array to hold command line arguments
    int bg;              // Flag to indicate if the job should run in background
    pid_t pid;           // Process ID
    sigset_t mask;       // Signal set for blocking/unblocking signals

    // Parse the command line and populate argv, 
    // also determine if the job should run in background
    bg = parseline(cmdline, argv);

    // Check if the command is a built-in shell command
    if(!builtin_cmd(argv)) { 
        
        // Prepare to block SIGCHLD signals to avoid race conditions
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigprocmask(SIG_BLOCK, &mask, NULL);

        // Create a child process
        if((pid = fork()) < 0){
            unix_error("forking error"); // If fork fails, report an error
        }
        // In the child process
        else if(pid == 0) {
            // Unblock SIGCHLD in the child process
            sigprocmask(SIG_UNBLOCK, &mask, NULL);
            // Set a unique process group ID for this child
            setpgid(0, 0);

            // Execute the command; if execvp fails, print an error
            if(execvp(argv[0], argv) < 0) {
                printf("%s: Command not found\n", argv[0]);
                exit(1); // Terminate the child process with an error status
            }
        } 
        // In the parent process
        else {
            // Add the job to the job list before unblocking signals
            if(!bg){
                addjob(jobs, pid, FG, cmdline); // Add as a foreground job
            }
            else {
                addjob(jobs, pid, BG, cmdline); // Add as a background job
            }
            // Unblock SIGCHLD signals
            sigprocmask(SIG_UNBLOCK, &mask, NULL);
            
            // If the job is a foreground job
            if (!bg){
                waitfg(pid); // Wait for the foreground job to complete
            } 
            // If the job is a background job
            else {
                // Print the background job details
                printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline);
            }
        }
    }
    // If it's a built-in command, the function builtin_cmd will handle it
    else {return;}
}


/////////////////////////////////////////////////////////////////////////////
//
// builtin_cmd - If the user has typed a built-in command then execute
// it immediately. The command name would be in argv[0] and
// is a C string. We've cast this to a C++ string type to simplify
// string comparisons; however, the do_bgfg routine will need 
// to use the argv array as well to look for a job number.
//
int builtin_cmd(char **argv) 
{
    // Check if the first argument is the "quit" command
    if (!strcmp(argv[0], "quit")) {
        exit(0); // Exit the program if "quit" is the command
    }
    // Check if the command is a single '&', which is a background job indicator
    // return 1 to indicate that it should not be treated as a command for the shell to execute.
    else if (!strcmp("&", argv[0])){
        return 1; // Return 1 to indicate this is not a built-in command
    }
    // Check if the command is "jobs"
    else if (!strcmp("jobs", argv[0])) {  
        listjobs(jobs);  // Call listjobs function to list all current jobs
        return 1;  // Return 1 to indicate the built-in command was handled
    }  
    // Check if the command is either "bg" or "fg"
    else if (!strcmp("bg", argv[0]) || !(strcmp("fg", argv[0]))) {  
        do_bgfg(argv);  // Call do_bgfg function to execute the bg/fg command
        return 1;  // Return 1 to indicate the built-in command was handled
    }  
    // If none of the above conditions are met, return 0 to indicate
    // that the command is not a built-in command and should be processed
    // as a regular program execution request.
    return 0;
}

/////////////////////////////////////////////////////////////////////////////
//
// do_bgfg - Execute the builtin bg and fg commands
//
void do_bgfg(char **argv) 
{   
    struct job_t *job; // Pointer to the job struct
    char *tmp;        // Temporary pointer to hold the argument value
    int jid;          // Job ID
    pid_t pid;        // Process ID

    tmp = argv[1];    // Assign the second argument to tmp
    
    // Check if the job/process ID argument is missing
    if(tmp == NULL) {
        printf("%s command requires PID or %%jobid argument\n", argv[0]);
        return; // Return from the function if no argument is provided
    }
    
    // Check if the argument is a job ID (starts with '%')
    if(tmp[0] == '%') {  
        jid = atoi(&tmp[1]); // Convert the job ID from string to integer
        job = getjobjid(jobs, jid); // Retrieve the job using the job ID
        if(job == NULL){  // If no job is found with the given ID
            printf("%s: No such job\n", tmp);  
            return;  
        }else{
            pid = job->pid; // Retrieve the process ID from the job struct
        }
    } 
    // Check if the argument is a process ID (starts with a digit)
    else if(isdigit(tmp[0])) { 
        pid = atoi(tmp); // Convert the process ID from string to integer
        job = getjobpid(jobs, pid); // Retrieve the job using the process ID
        if(job == NULL){  // If no job is found with the given PID
            printf("(%d): No such process\n", pid);  
            return;  
        }  
    }  
    // If the argument is neither a job ID nor a process ID
    else {
        printf("%s: argument must be a PID or %%jobid\n", argv[0]);
        return; // Return from the function if the argument is invalid
    }
    // Send the SIGCONT signal to the job/process to continue running it
    kill(-pid, SIGCONT);
    
    // If the command is 'fg' (foreground)
    if(!strcmp("fg", argv[0])) {
        job->state = FG; // Change the job state to foreground
        waitfg(job->pid); // Wait for the foreground job to complete
    } 
    // If the command is 'bg' (background)
    else{
        printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline); // Print the job details
        job->state = BG; // Change the job state to background
    } 
}

/////////////////////////////////////////////////////////////////////////////
//
// waitfg - Block until process pid is no longer the foreground process
//
void waitfg(pid_t pid)
{
    struct job_t* job; // Declare a pointer to a job_t struct

    // Retrieve the job corresponding to the given process ID
    job = getjobpid(jobs, pid);

    // Check if the given process ID is valid. If pid is 0, it indicates
    // that there is no such process, so the function returns immediately
    if(pid == 0){
        return;
    }

    // If the job corresponding to the given PID exists
    if(job != NULL){
        // The loop continues to run as long as the process ID
        // passed to the function is the same as the process ID
        // of the current foreground job. This effectively makes
        // the shell wait (busy wait) for the foreground job to finish.

        while(pid == fgpid(jobs)){
            // Empty busy-wait loop
        }
    }

    // Once the foreground job is no longer the job with the given PID
    // (which means it has finished), this function returns, allowing
    // the shell to continue executing.
    return;
}

/////////////////////////////////////////////////////////////////////////////
//
// Signal handlers
//


/////////////////////////////////////////////////////////////////////////////
//
// sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
//     a child job terminates (becomes a zombie), or stops because it
//     received a SIGSTOP or SIGTSTP signal. The handler reaps all
//     available zombie children, but doesn't wait for any other
//     currently running children to terminate.  
//
void sigchld_handler(int sig) 
{
    int status;  // Variable to store the status information of the child process
    pid_t pid;   // Variable to store the process ID
    
    // Continuously reap child processes that have changed state
    while ((pid = waitpid(fgpid(jobs), &status, WNOHANG|WUNTRACED)) > 0) {  
        // Check if the child process was stopped by a signal
        if (WIFSTOPPED(status)){ 
            // Update the job's state in the job list to indicate it is stopped
            getjobpid(jobs, pid)->state = ST;
            int jid = pid2jid(pid); // Get the job ID associated with the pid
            // Print message indicating the job has been stopped
            printf("Job [%d] (%d) Stopped by signal %d\n", jid, pid, WSTOPSIG(status));
        }  
        // Check if the child process was terminated by a signal
        else if (WIFSIGNALED(status)){
            int jid = pid2jid(pid); // Get the job ID associated with the pid
            // Print message indicating the job has been terminated by a signal
            printf("Job [%d] (%d) terminated by signal %d\n", jid, pid, WTERMSIG(status));
            // Remove the job from the job list
            deletejob(jobs, pid);
        }  
        // Check if the child process exited normally
        else if (WIFEXITED(status)){  
            // Remove the job from the job list if it exited normally
            deletejob(jobs, pid);  
        }  
    }  
    // Return from the function after handling all child processes that changed state
    return; 
}

/////////////////////////////////////////////////////////////////////////////
//
// sigint_handler - The kernel sends a SIGINT to the shell whenver the
//    user types ctrl-c at the keyboard.  Catch it and send it along
//    to the foreground job.  
//
void sigint_handler(int sig) 
{
    pid_t pid = fgpid(jobs);  // Get the PID of the foreground job
    
    // Check if there is a valid foreground process.
    // If pid is not 0, it means there is a foreground process.
    if (pid != 0) {     
        // Send the SIGINT signal to the entire foreground process group.
        // Use kill with -pid, which sends the signal to all
        // processes in the process group whose ID is equal to pid.
        // This stops all parts of potentially pipelined or otherwise grouped commands in the foreground job.
        kill(-pid, sig);
    }   
    // Return from the handler function. No further action is required
    // if there was no foreground process, or after the signal has been sent.
    return;   
}

/////////////////////////////////////////////////////////////////////////////
//
// sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
//     the user types ctrl-z at the keyboard. Catch it and suspend the
//     foreground job by sending it a SIGTSTP.  
//
void sigtstp_handler(int sig) 
{
    pid_t pid = fgpid(jobs);  // Get the PID of the foreground job
    
    // Check if there is a valid foreground process.
    // If pid is not 0, it means there is a foreground process running.
    if (pid != 0) { 
        // Send the SIGTSTP signal to the entire foreground process group.
        // Use kill with -pid, which sends the signal to all
        // processes in the process group whose ID is equal to pid.
        // This stops all parts of potentially pipelined or otherwise grouped commands in the foreground job.
        kill(-pid, sig);  
    }  
    // Return from the handler function. The action is complete if there
    // was a foreground job, and no action is needed if there wasn't.
    return;   
}

/*********************
 * End signal handlers
 *********************/




