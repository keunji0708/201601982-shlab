/* 
 * tsh - A tiny shell program with job control
 *
 *
 * <김은지 b201601982>
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */
/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */


/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
struct job_t {              /* The job struct */
	pid_t pid;              /* job PID */
	int jid;                /* job ID [1, 2, ...] */
	int state;              /* UNDEF, BG, FG, or ST */
	char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */

extern char **environ;      /* defined in libc */
char prompt[] = "eslab_tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void waitfg(pid_t pid, int output_fd);
void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs, int output_fd);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);


/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
	char c;
	char cmdline[MAXLINE];
	int emit_prompt = 1; /* emit prompt (default) */

	/* Redirect stderr to stdout (so that driver will get all output
	 * on the pipe connected to stdout) */
	dup2(1, 2);

	/* Parse the command line */
	while ((c = getopt(argc, argv, "hvp")) != EOF) {
		switch (c) {
			case 'h':             /* print help message */
				usage();
				break;
			case 'v':             /* emit additional diagnostic info */
				verbose = 1;
				break;
			case 'p':             /* don't print a prompt */
				emit_prompt = 0;  /* handy for automatic testing */
				break;
			default:
				usage();
		}
	}

	/* Install the signal handlers */

	/* These are the ones you will need to implement */
	Signal(SIGINT,  sigint_handler);   /* ctrl-c */
	Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
	Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */
	Signal(SIGTTIN, SIG_IGN);
	Signal(SIGTTOU, SIG_IGN);

	/* This one provides a clean way to kill the shell */
	Signal(SIGQUIT, sigquit_handler); 

	/* Initialize the job list */
	initjobs(jobs);

	/* Execute the shell's read/eval loop */
	while (1) {

		/* Read command line */
		if (emit_prompt) {
			printf("%s", prompt);
			fflush(stdout);
		}
		if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
			app_error("fgets error");
		if (feof(stdin)) { /* End of file (ctrl-d) */
			fflush(stdout);
			fflush(stderr);
			exit(0);
		}

		/* Evaluate the command line */
		eval(cmdline);
		fflush(stdout);
		fflush(stdout);
	} 

	exit(0); /* control never reaches here */
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child proces s must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
 */
void eval(char *cmdline) 
{
	char *argv[MAXARGS];  // command 저장
	char buf[MAXLINE];
	int bg;
	pid_t pid;
	sigset_t mask;

	strcpy(buf, cmdline);
	bg = parseline(buf, argv);

	sigemptyset(&mask);
	// 인자로 주어진 시그널 set에 포함되어 있는 모든 시그널을 삭제

	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGCHLD);
	sigaddset(&mask, SIGTSTP);
	// 시그널 번호가 SIGINT,SIGCHLD,SIGTSTP인 시그널을 시그널 set에 추가

	if (argv[0] == NULL)
		return;

	if(!builtin_cmd(argv)){
		//buildin_cmd가 아닌 경우, Check Child Process
		sigprocmask(SIG_BLOCK, &mask, NULL);//Block SIGCHLD
		
		if((pid = fork()) < 0)
			unix_error("fork error");
		
		else if(pid == 0) {//Child Process인 경우, execve() 수행
			sigprocmask(SIG_UNBLOCK, &mask, NULL);//Unblock SIGCHLD
			/*자식은 부모의 blocked set을 상속받기 때문에 unblock
			 SIGCHILD를 해주어야 함*/
			setpgid(0, 0);
			// 프로세스에 프로세스 그룹 아이디를 세팅
			// pid로 설정된 process의 process group의 ID를 pgid로 설정
			if (execve(argv[0], argv, environ) < 0) {
				printf("%s : Command not found\n", argv[0]);
				exit(0);
			}
		}

		if (!bg) { // Foreground 일때,
			addjob(jobs, pid, FG, cmdline); // joblist에 job 추가
			sigprocmask(SIG_UNBLOCK, &mask, NULL); // Unblock SIGCHLD
			waitfg(pid, 1); // Race Condition
			/*int status;
			  if (waitpid(pid, & status, 0) < 0){
			 	 unix_error ("waitfg : waitpid error:);
			  }
			  deletejob(jobs, pid);
			  */
		}

		else { // Background 일때,
			addjob(jobs, pid, BG, cmdline); // joblist에 job 추가
			sigprocmask(SIG_UNBLOCK, &mask, NULL);// Unblock SIGCHLD
			printf("(%d) (%d) %s", pid2jid(pid),(int)pid, cmdline);
		}
	}
	return;
}

int builtin_cmd(char **argv)
{
	char *cmd = argv[0];
	struct job_t *j;

	if(!strcmp(cmd, "quit")){ /* quit commad */
		exit(0);
	}
	if (!strcmp(cmd,"jobs")){ /* jobs command */
		listjobs(jobs,1);
		return 1;
	}
	if (!strcmp(cmd, "&")) {
		return 1;
	}
	if ((!strcmp(cmd, "bg")) | (!strcmp(cmd, "fg"))){
		int jobid = (argv[1][0] == '%' ? 1 : 0); // JID인지 PID인지 확인
		if (jobid == 1){
			if ((j = getjobjid(jobs, atoi(&argv[1][1]))) == NULL){
				// 주어진 Job ID가 없는 경우
				printf("%s : No such job\n", argv[1]);
			}
		}
		else {
			if ((j = getjobpid(jobs, atoi(argv[1]))) == NULL){
				// 주어진 Process ID가 없는 경우
				printf("(%d) : No such process\n", atoi(argv[1]));
			}
		}	
		if (!strcmp(cmd, "bg")){ /* bg command*/
			j->state = BG; // state를 BG로 바꿔줌
			printf("[%d] (%d) %s", j->jid, j->pid, j->cmdline);
			kill(-j->pid, SIGCONT); // SIGCONT를 이용, 중지된 작업 다시 시작
			return 1;
		}
		if (!strcmp(cmd, "fg")){ /* fg command*/
			j->state = FG; // state를 FG로 바꿔줌
			kill(-j->pid, SIGCONT);// SIGCONT를이용, 중지된 작업 다시 시작
			waitfg(j->pid, 1); // Job이 끝날때까지 기다림 
			return 1;
		}
	}
	return 0; /* not a builtin command */
}

void waitfg(pid_t pid, int output_fd)
{
	struct job_t *j = getjobpid(jobs, pid);
	char buf[MAXLINE];

	// The FG job has already completed and been reaped by the andler
	if (!j)
		return;

	/*
	* Wait for process pid to longer be the foregrnund process.
	* Note: Using pause() instead of sleep() would introduce a race
	* that couldecnuse us to miss the signal
	*/
	while (j->pid == pid && j->state == FG)
		sleep(1);

	if (verbose) {
		memset(buf, '\0', MAXLINE);
		sprintf(buf, "waitfg: Process (%d) no longer the fg process:q\n", pid);
		if (write(output_fd, buf, strlen(buf)) < 0) {
			fprintf(stderr, "Error writing to file\n");
			exit(1);
		}
	}
	return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{		
	struct job_t *j;
	int status;
	pid_t pid;

	/*Child Process가 종료될때까지 기다린후, Parent Process 처리*/
	while ((pid = waitpid(-1, &status, WUNTRACED|WNOHANG)) > 0){

		/*Child Process가 어떤 signal에 의해  종료된 경우*/
		if(WIFSIGNALED(status)){
			printf("Job [%d] (%d) terminated by signal %d\n", pid2jid(pid), (int)pid, WTERMSIG(status));
			deletejob(jobs, pid);
		}

		/*Child가 정지된 경우*/
		else if(WIFSTOPPED(status)){
			j = getjobpid(jobs, pid);
			j->state = ST; // job의 상태를 stop으로 변경
			printf("Job [%d] (%d) stopped by signal %d\n", pid2jid(pid), (int)pid, WSTOPSIG(status));
		}

		/*자식이 정상적으로 종료된 경우, 0이 아닌 값 리턴*/
		else if(WIFEXITED(status)) {
			deletejob(jobs, pid); // Delete the child form job list
		}
	}
	return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
	pid_t pid = fgpid(jobs); //FG 작업 pid

	if (pid > 0){
		kill(-pid, SIGINT); // FG 작업에만 SIGINT가 처리
	} // kill()는 지정한 pid를 가지는 process에 signal 전달
	return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
	pid_t pid = fgpid(jobs); // FG 작업 pid

	if (pid > 0){
		kill(-pid, SIGTSTP); // FG 작업에만 SIGTSTP가 처리
	}
	return;
}

/*********************
 * End signal handlers
 *********************/


/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{

	static char array[MAXLINE]; /* holds local copy of command line */
	char *buf = array;          /* ptr that traverses command line */
	char *delim;                /* points to first space delimiter */
	int argc;                   /* number of args */
	int bg;                     /* background job? */

	strcpy(buf, cmdline);
	buf[strlen(buf)-1] = ' '; /* replace trailing '\n' with space */
	while(*buf && (*buf == ' '))
		buf++;

	/* Build the argv list */
	argc = 0;
	if (*buf == '\'') {
		buf++;
		delim = strchr(buf, '\'');
	}
	else {
		delim = strchr(buf, ' ');
	}

	while (delim) {
		argv[argc++] = buf;
		*delim = '\0';
		buf = delim + 1;
		while (*buf && (*buf == ' ')) /* ignore spaces */
			buf++;

		if (*buf == '\'') {
			buf++;
			delim = strchr(buf, '\'');
		}
		else {
			delim = strchr(buf, ' ');
		}
	} 

	argv[argc] = NULL;

	if (argc == 0)  /* ignore blank line */
		return 1;

	/* should the job run in the background? */
	if ((bg = (*argv[argc-1] == '&')) != 0)
		argv[--argc] = NULL;

	return bg;

}

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
	int i;

	for (i = 0; i < MAXJOBS; i++)
		clearjob(&jobs[i]);
}

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
	job->pid = 0;
	job->jid = 0;
	job->state = UNDEF;
	job->cmdline[0] = '\0';
}


/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
	int i, max=0;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid > max)
			max = jobs[i].jid;
	return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
	int i;

	if (pid < 1)
		return 0;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == 0) {
			jobs[i].pid = pid;
			jobs[i].state = state;
			jobs[i].jid = nextjid++;
			if (nextjid > MAXJOBS)
				nextjid = 1;
			strcpy(jobs[i].cmdline, cmdline);
			if(verbose){
				printf("Added job. [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
			}
			return 1;
		}
	}
	printf("Tried to create too many jobs\n");
	return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
	int i;

	if (pid < 1)
		return 0;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == pid) {
			clearjob(&jobs[i]);
			nextjid = maxjid(jobs)+1;
			return 1;
		}
	}
	return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
	int i;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].state == FG)
			return jobs[i].pid;
	return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
	int i;

	if (pid < 1)
		return NULL;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid)
			return &jobs[i];
	return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
	int i;

	if (jid < 1)
		return NULL;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid == jid)
			return &jobs[i];
	return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
	int i;

	if (pid < 1)
		return 0;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid) {
			return jobs[i].jid;
		}
	return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs, int output_fd) 
{
	int i;
	char buf[MAXLINE];

	for (i = 0; i < MAXJOBS; i++) {
		memset(buf, '\0', MAXLINE);
		if (jobs[i].pid != 0) {
			sprintf(buf, "(%d) (%d) ", jobs[i].jid, jobs[i].pid);
			if(write(output_fd, buf, strlen(buf)) < 0) {
				fprintf(stderr, "Error writing to output file\n");
				exit(1);
			}
			memset(buf, '\0', MAXLINE);
			switch (jobs[i].state) {
				case BG:
					sprintf(buf, "Running    ");
					break;
				case FG:
					sprintf(buf, "Foreground ");
					break;
				case ST:
					sprintf(buf, "Stopped    ");
					break;
				default:
					sprintf(buf, "listjobs: Internal error: job[%d].state=%d ",
							i, jobs[i].state);
			}
			if(write(output_fd, buf, strlen(buf)) < 0) {
				fprintf(stderr, "Error writing to output file\n");
				exit(1);
			}
			memset(buf, '\0', MAXLINE);
			sprintf(buf, "%s", jobs[i].cmdline);
			if(write(output_fd, buf, strlen(buf)) < 0) {
				fprintf(stderr, "Error writing to output file\n");
				exit(1);
			}
		}
	}
	if(output_fd != STDOUT_FILENO)
		close(output_fd);
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
	printf("Usage; shell [-hvp]\n");
	printf("   -h   print this message\n");
	printf("   -v   print additional diagnostic information \n");
	printf("   -p   do not emit a command prompt \n");
	exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
	fprintf(stdout, "%s: %s\n", msg, strerror(errno));
	exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
	fprintf(stdout, "%s\n", msg);
	exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
	struct sigaction action, old_action;

	action.sa_handler = handler;  
	sigemptyset(&action.sa_mask); /* block sigs of type being handled */
	action.sa_flags = SA_RESTART; /* restart syscalls if possible */

	if (sigaction(signum, &action, &old_action) < 0)
		unix_error("Signal error");
	return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
	printf("Terminating after receipt of SIGQUIT signal\n");
	exit(1);
}

