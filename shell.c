#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"



/* Array of paths..!*/
static char *PATH[] = {"/usr/local/sbin/",
                       "/usr/local/bin/",
                       "/usr/sbin/",
                       "/usr/bin/",
                       "/sbin/",
                       "/bin/",
                       NULL};

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

int cmd_exit(struct tokens *tokens);
int cmd_help(struct tokens *tokens);
int cmd_pwd(struct tokens *tokens);
int cmd_cd(struct tokens *tokens);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens *tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc
{
  cmd_fun_t *fun;
  char *cmd;
  char *doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
    {cmd_help, "?", "show this help menu"},
    {cmd_exit, "exit", "exit the command shell"},
    {cmd_pwd, "pwd", "show the current directory"},
    {cmd_cd, "cd", "change directory"}};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens *tokens)
{
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens *tokens)
{
  exit(0);
}

/* Print the current directory */
int cmd_pwd(unused struct tokens *tokens)
{
  // what to do when the name is too fucking long?
  // so supposing the length should less than 256
  unsigned int max_size = 256;
  char *buffer = malloc(max_size);
  if (buffer)
  {
    char *ans = getcwd(buffer, max_size);
    if (!ans)
    {
      free(buffer);
      return -1;
    }
    printf("%s\n", buffer);
    free(buffer);
    return 0;
  }
  return -1;
}

/* change directory */
int cmd_cd(struct tokens *t)
{
  int length = tokens_get_length(t);
  if (length < 2)
  {
    return 0;
  }

  char *directory = tokens_get_token(t, 1);
  int ans = chdir(directory);
  if (ans < 0)
  {
    // something goes wrong
    printf("cd %s: no such file or directory\n", directory);
    return -1;
  }
  return 0;
}

int background_execute(struct tokens *tokens)
{
  pid_t pid = fork();
  if (pid < 0)
  {
    printf("Can't fork...\n");
    return -1;
  }
  else if (pid == 0)
  {
    // child process
    pid = getpid();
    int length = tokens_get_length(tokens) - 1;
    setpgrp();
    char *argv[length + 1];
    for (int i = 0; i < length; i++)
    {
      argv[i] = tokens_get_token(tokens, i);
    }
    argv[length] = NULL;

    int path_len = strlen(argv[0]);
    char path[path_len];
    strcpy(path, argv[0]);
    // if in the current adress there is the program
    int ans = execve(argv[0], argv, 0);
    if (ans > 0)
    {
      exit(0);
    }

    for (int j = 0; PATH[j] != NULL; j++)
    {
      strcpy(argv[0], PATH[j]);
      strcat(argv[0], path);

      int ans = execve(argv[0], argv, 0);
      if (ans > 0)
      {
        // stop itself
        kill(pid, SIGTSTP);
        exit(0);
      }
    }
    // didn't find the program return 2
    printf("This shell doesn't know how to run this stuff.\n");
    exit(2);
  }
  else
  {
    printf("%d\n", pid);
    return 0;
  }
}

/* execute program..! */
int cmd_execute(struct tokens *tokens)
{
  int length = tokens_get_length(tokens);
  // check if it's background process
  if (length != 0 && !strcmp(tokens_get_token(tokens, length - 1), "&"))
  {
    return background_execute(tokens);
  }
  pid_t pid = fork();
  if (pid < 0)
  {
    printf("Can't fork...\n");
    return -1;
  }
  else if (pid == 0)
  {
    // Child process

    // set new process group
    setpgrp();
    if (length == 0) // if the input is empty
      exit(0);

    char *argv[length + 1];
    for (int i = 0; i < length; i++)
    {
      argv[i] = tokens_get_token(tokens, i);
    }
    argv[length] = NULL;
    // TODO envp???

    // add path name
    int path_len = strlen(argv[0]);
    char path[path_len];
    strcpy(path, argv[0]);
    // if in the current adress there is the program
    int ans = execve(argv[0], argv, 0);
    if (ans > 0)
    {
      exit(0);
    }

    for (int j = 0; PATH[j] != NULL; j++)
    {
      strcpy(argv[0], PATH[j]);
      strcat(argv[0], path);

      int ans = execve(argv[0], argv, 0);
      if (ans > 0)
      {
        exit(0);
      }
    }
    // didn't find the program return 2
    printf("This shell doesn't know how to run this stuff.\n");
    exit(2);
  }
  else
  {
    tcsetpgrp(0, pid);
    // parent process
    int status;
    waitpid(pid, &status, WUNTRACED);
    if (status == 2)
    {
      tcsetpgrp(0, getpgrp());
      return 0;
    }
    tcsetpgrp(0, getpgrp());
    return 0;
  }
}

void INOUT_handler(int signum)
{
  signal(signum, SIG_IGN);
  raise(signum);
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[])
{
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell()
{
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive)
  {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

int main(unused int argc, unused char *argv[])
{
  signal(SIGTTOU, INOUT_handler);
  signal(SIGTTIN, INOUT_handler);

  init_shell();

  static char line[4096];
  int line_num = 0;
  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  /*struct tokens *tokens = tokenize("python3 &");
  cmd_execute(tokens);*/

  // test run program
  /*struct tokens *tokens = tokenize("/usr/bin/wc shell.c");
  cmd_execute(tokens);

  struct tokens *tokens = tokenize("wc shell.c");
  cmd_execute(tokens);*/
  while (fgets(line, 4096, stdin))
  {
    /* Split our line into words. */
    struct tokens *tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0)
    {
      cmd_table[fundex].fun(tokens);
    }
    else
    {
      /* REPLACE this to run commands as programs. */
      //fprintf(stdout, "This shell doesn't know how to run programs.\n");
      cmd_execute(tokens);
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
