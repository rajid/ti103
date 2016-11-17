#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <termios.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>

typedef int	boolean;

#define TRUE	1
#define FALSE	0
#define TI103_READ_BUFFER	"$>2800008C#"
#define TI103_REPLY_GOOD	"$<2800!"
#define TI103_REPLY_NULL	"$<2800!4B#"

#define TI103_ON_CMD_SIZE	13 /* "J10J10 JONJON" */
#define TI103_OFF_CMD_SIZE	15 /* "J10J10 JOFFJOFF" */

#define TI103_COMMAND		"$>28001"
#define TI103_CKSUM		"CC#" /* Fake cksum */

#define MAX_OUTSTANDING_CMDS	50

#define MIN_RESET_INTERVAL	10	/* seconds */

typedef enum {
  undefined,
  state_on,
  state_off,
} state_value;

typedef enum {
  no_function,
  on,
  off,
  dim,
  bright,
  all_lights_on,
  all_units_off,
  all_lights_off,
  hail_request,
  hail_ack,
  preset_dim_0,
  preset_dim_1,
  status_on,
  status_off,
  status_request,
} func_value;

typedef enum {
  NONE,
  TRANSITION,
  ALWAYS,
  NEVER,
} when_t;

typedef struct x10_trigger_ {
  struct x10_trigger_	*next;
  int			house;
  int			unit;
  func_value		function;
  when_t		when;
  char			*command;
} x10_trigger;

typedef struct x10_desc_ {
  struct x10_desc_	*next;
  int			house;
  int			unit;
  char			*description;
} x10_desc;

boolean		verbose=FALSE;
int		tty;
FILE		*log_file;
FILE		*stdin_file;
FILE		*events_fd;
x10_trigger	*trigger_list;
x10_desc	*desc_list;

//char 	*device="/dev/ttyUSB0";
char 	*device="/dev/tty.KeySerial1";
//char 	*device="/dev/cu.usbserial-A1023OBJ";
char	ti103_output[512];
char	*ti103_output_ptr;
/* Remember - unit numbers are 1-16, arrays start at 0 */
int	house_unit[17];
state_value	states[17][17];
boolean	ti103_read_waiting;
char	*command_file=NULL;
char	*events_file=NULL;

/*
 * We save ti103 commands which we have written so we can verify
 * that we actually see them on the wire.  If we don't,
 * then it may mean that the ti103 device has been unplugged.
 */
typedef struct ti103_commands_ {
  char		house;
  int		unit;
  func_value	func;
} ti103_commands;
ti103_commands ti103_cmds[MAX_OUTSTANDING_CMDS];
int ti103_cmds_write;
int ti103_cmds_verify;

static void dump_and_exit(int);
static boolean function_trigger (int, int, func_value, when_t);
static void all_trigger (int, int, func_value);
void perform_trigger (x10_trigger *, int, int, func_value);

/*
 * Translate a house code value to a character.
 * Make house code "A" be "1"
 * Don't use house number 0.
 */
static char
house2name (int house) {

  if (house >= 1 && house <= 16)
    return '@' + house;

  return '@';
}

/*
 * Translate a house code letter to a value.
 * Make house code "A" be "1"
 * Don't use house number 0.
 */
static char
name2house (char name) {

  if (islower(name))
    name = toupper(name);
  if (name >= 'A' && name <= 'P')
    return name - '@';

  return 0;			/* internal error! */
}



/*
 * Initialize the TI103 device serial line.
 * If we can't open the device, then "tty" is set to "-1".
 * Code elsewhere does special test functions when "tty" is "-1".
 * This means you can test the program on a system without a TI103
 * to verify functionality.
 */
static int
init_ti103 (void) {
  struct termios	termios;
  int			state;
  int			i;

  /*
   * Init. our indexes for saving written commands
   * and for verifying them.
   */
  ti103_cmds_write = 0;
  ti103_cmds_verify = 0;
  for (i = 0; i < MAX_OUTSTANDING_CMDS; i++) {
    ti103_cmds[i].house = name2house('A');
    ti103_cmds[i].unit = 0;
    ti103_cmds[i].func = no_function;
  }

  tty = open(device, O_NONBLOCK | O_RDWR);
  if (tty == -1) {
    fprintf(log_file, "Can't open Ti103 device - errno=%d\n", errno);
    return -1;			/* no such device present */
  }

  /*
   * Set speed
   */
  if (tcgetattr(tty, &termios) < 0) {
    fprintf(log_file, "! Error getting speed errno=%d\n", errno);
    return -1;
  }

  cfsetspeed(&termios, B9600);
  termios.c_cflag &= ~(CCAR_OFLOW|CDSR_OFLOW|CDTR_IFLOW|CRTS_IFLOW);
  termios.c_cflag |= CLOCAL;

  if (tcsetattr(tty, TCSANOW, &termios) < 0) {
    fprintf(log_file, "! Error getting speed errno=%d\n", errno);
    return -1;
  }

  return tty;
}

/*
 * Init's
 * Open a log file.
 * Open stdin with fopen, so we an use fflush
 * Try to catch as many signals as possible so we can dump
 * commands and save state.
 */
static void
init (void) {

  ti103_output_ptr = &ti103_output[0];
  bzero(house_unit, sizeof(house_unit));
  bzero(states, sizeof(states));
  ti103_read_waiting = FALSE;

  log_file = fdopen(STDOUT_FILENO, "a");
  setlinebuf(log_file);

  stdin_file = fdopen(STDIN_FILENO, "r");

  trigger_list = NULL;
  tty = -1;

  /*
   * Catch some signals
   */
  signal(SIGHUP, dump_and_exit);
  signal(SIGINT, dump_and_exit);
  signal(SIGQUIT, dump_and_exit);
  signal(SIGTERM, dump_and_exit);
}

/*
 * Write a command to the Ti103
 */
static boolean
write_ti103 (char *command, int size) {
  int	ret;

  if (tty == -1)
    return FALSE;

  if (verbose)
    fprintf(log_file, "Writing ti103(%d): '%s'\n", tty, command);

  ret = write(tty, command, size);
  if (ret != size) {
    fprintf(log_file, "! Got '%d' when I wrote '%d' bytes (%d)\n",
	    ret, size, errno);
    return FALSE;
  }
  fflush(log_file);

  return TRUE;
}

/*
 * Write command to Ti103 which tells it to send us whatever
 * X10 commands it has saved in its buffer.  We do this every
 * 5 seconds on an alarm signal
 */
static void
write_ti103_read_buffer (int sig) {

  if (tty == -1)
    return;

  write_ti103(TI103_READ_BUFFER, sizeof(TI103_READ_BUFFER)-1);
  ti103_read_waiting = TRUE;

  alarm(5);
}


/*
 * Translate a "state" to a character string for printing.
 */
char *
state2name (state_value state) {
  switch (state) {
  case state_on:
    return "On";

  case state_off:
    return "Off";

  case undefined:
    return "Null";
  }
}


/*
 * Translate a function to a character string for printing
 */
static char *
func2name (func_value func) {

  switch ( func ) {
  case no_function:
    return "";
  case on:
    return "ON";
  case off:
    return "OFF";
  case dim:
    return "DIM";
  case bright:
    return "BGT";
  case all_lights_on:
    return "ALN";
  case all_units_off:
    return "AUF";
  case all_lights_off:
    return "ALF";
  case hail_request:
    return "HRQ";
  case hail_ack:
    return "HAK";
  case preset_dim_0:
    return "PR0";
  case preset_dim_1:
    return "PR1";
  case status_on:
    return "SON";
  case status_off:
    return "SOF";
  case status_request:
    return "SRQ";
  }
}


/*
 * Parses an x10 function from a character string to a function value.
 * Side-effects:
 *	sets "*astate" if func was on of off
 * Considerations:
 *	input string MUST be in CAPS
 */
static func_value
parse_ti103_function (state_value *astate, char **cp) {
  char		*cptr;
  state_value	new_state;
  func_value	function;

  cptr = *cp;
  new_state = undefined;	/* init. */

  if (strncmp(cptr, "ON", 2) == 0) {
    function = on;
    new_state = state_on;
    cptr += 2;
  } else if (strncmp(cptr, "OFF", 3) == 0) {
    function = off;
    new_state = state_off;
    cptr += 3;
  } else if (strncmp(cptr, "DIM", 3) == 0) {
    function = dim;
    cptr += 3;
  } else if (strncmp(cptr, "BGT", 3) == 0) {
    function = bright;
    new_state = state_on;
    cptr += 3;
  } else if (strncmp(cptr, "ALN", 3) == 0) {
    function = all_lights_on;
    cptr += 3;
  } else if (strncmp(cptr, "AUF", 3) == 0) {
    function = all_units_off;
    cptr += 3;
  } else if (strncmp(cptr, "ALF", 3) == 0) {
    function = all_lights_off;
    cptr += 3;
  } else if (strncmp(cptr, "HRQ", 3) == 0) {
    function = hail_request;
    cptr += 3;
  } else if (strncmp(cptr, "HAK", 3) == 0) {
    function = hail_ack;
    cptr += 3;
  } else if (strncmp(cptr, "PR0", 3) == 0) {
    function = preset_dim_0;
    cptr += 3;
  } else if (strncmp(cptr, "PR1", 3) == 0) {
    function = preset_dim_1;
    cptr += 3;
  } else if (strncmp(cptr, "SON", 3) == 0) {
    function = status_on;
    new_state = state_on;
    cptr += 3;
  } else if (strncmp(cptr, "SOF", 3) == 0) {
    function = status_off;
    new_state = state_off;
    cptr += 3;
  } else if (strncmp(cptr, "SRQ", 3) == 0) {
    function = status_request;
    cptr += 3;
  } else {
    *astate = undefined;
    return no_function;
  }

  *astate = new_state;
  *cp = cptr;

  return function;
}

/*****************************
 * Description command support
 */

/*
 * Add a new description to the linked list of descriptions by
 * house code and unit number.
 */
static x10_desc *
new_desc (int house, int unit, char *cp) {
  x10_desc	*desc;

  desc = malloc(sizeof(*desc));
  if (!desc)
    return NULL;

  desc->house = house;
  desc->unit = unit;
  desc->description = malloc(strlen(cp)+1);
  if (!desc->description) {
    free(desc);
    return NULL;
  }
  strcpy(desc->description, cp);

  fprintf(log_file, "New Desc: %c%d: %s\n",
	  house2name(house), unit, cp);
  fflush(log_file);

  return desc;
}


/*
 * Lookup a description given the house code and unit number
 */
static x10_desc *
find_desc (int house, int unit) {
  x10_desc	*desc;

  for (desc = desc_list ; desc ; desc = desc->next) {
    if (desc->house == house && desc->unit == unit) {
      return desc;
    }
  }

  return NULL;
}

/*
 * Remove a description from the linked list
 */
static void
remove_desc (x10_desc *desc) {
  x10_desc	*d;

  if (desc_list == desc) {	/* remove first one */
    desc_list = desc_list->next;
    free(desc);
    return;
  }

  for (d = desc_list ; d->next ; d = d->next) {
    if (d->next == desc) {
      d->next = d->next->next;
      free(desc);
      return;
    }
  }

  return;
}

/*
 * Insert a new description for a house code and unit number into
 * the list.
 */
static void
insert_desc(int house, int unit, char *cp) {
  x10_desc	*desc;
  
  desc = new_desc(house, unit, cp);
  if (!desc)
    return;

  /*
   * Insert into linked list
   */
  desc->next = desc_list;
  desc_list = desc;
}

/*
 * Replace a description with a new one.
 */
static void
replace_desc (x10_desc *desc, char *des) {

  free(desc->description);
  
  if (strlen(des) > 0) {
    desc->description = malloc(strlen(des)+1);
    strcpy(desc->description, des);
  } else {			/* remove it instead */
    remove_desc(desc);
  }
}

/*
 Add a new description to the linked list.
 */
static void
add_desc (int house, int unit, char *cp) {
  x10_desc	*desc;

  desc = find_desc(house, unit);
  if (!desc) {
    insert_desc(house, unit, cp);
  } else {
    replace_desc(desc, cp);
  }
}


/*
 * Print out all descriptions in the linked list using the same
 * style as would be expected as the input command.
 */
static void
print_descs (FILE *file) {
  x10_desc	*desc;

  for (desc = desc_list ; desc ; desc = desc->next) {
    fprintf(file, "desc %c%2d %s\n",
	    house2name(desc->house), desc->unit,
	    desc->description);
  }
  fflush(file);
}

/*
 * Look for a description for this house code and unit number.
 * If we don't find one, create a generic one on-the-fly.
 */
static char *
x10_description (char house, int unit) {
  static char	buffer[80];
  x10_desc	*desc;

  desc = find_desc(house, unit);
  if (desc) {
    return desc->description;
  }

  snprintf(buffer, sizeof(buffer)-1, "%c %2d",
	   house2name(house), unit);
  return buffer;
}

/*
 * Copy from "command" into "buffer", while handling special substitutions.
 * Don't use more than "bufsize" characters in the output.
 * Substitutions allowed are:
 * %h = house code
 * %u = unit number
 * %f = function code
 * %d = description string associated with house and unit 
 */
static void
substitute_characters(char *buffer, int bufsize, char *command,
		      int house, int unit, func_value func) {
  char		*cp1, *cp2;
  int		count;

  cp1 = buffer;
  cp2 = command;
  while ( *cp2 ) {
    if (*cp2 != '%') {
      *cp1++ = *cp2++;
      bufsize--;
    } else {
      cp2++;
      bufsize--;
      if (*cp2 == 'h') {
	cp2++;
	*cp1++ = house2name(house);
	bufsize--;
      } else if (*cp2 == 'u') {
	cp2++;
	count = sprintf(cp1, "%d", unit);
	cp1 += count;
	bufsize -= count;
      } else if (*cp2 == 'f') {
	cp2++;
	count = sprintf(cp1, "%s", func2name(func));
	cp1 += count;
	bufsize -= count;
      } else if (*cp2 == 'd') {
	cp2++;
	count = sprintf(cp1, "%s", x10_description(house, unit));
	cp1 += count;
	bufsize -= count;
      }
    }
  }

  *cp1 = '\0';
}


/************************************************************************
 * Action triggers designated by house code, unit number, and function. *
 ************************************************************************/

/*
 * Search through our linked list of trigger structs looking for the
 * particular house code, unit number, and function passed.
 */
static x10_trigger *
find_trigger (int house, int unit, func_value function, when_t when) {
  x10_trigger	*trigger;

  for (trigger = trigger_list ; trigger ; trigger = trigger->next) {
    if (trigger->house == house && trigger->unit == unit &&
	when == NEVER && trigger->when == NEVER) {
	/* found directive to "never" do this trigger */
      return trigger;
    } else if (trigger->house == house && trigger->unit == unit &&
	       trigger->function == function &&
	       (trigger->when == when || when == NONE)) {
      /* found directive matching this trigger */
      return trigger;
    }
  }

  return NULL;
}

/*
 * Remove a trigger from the linked list
 */
static void
remove_trigger (x10_trigger *trigger) {
  x10_trigger	*t;

      fprintf(log_file, "remove trigger %c%02d %-4s\n",
	      house2name(trigger->house), trigger->unit,
	      func2name(trigger->function));

  if (trigger_list == trigger) {	/* remove first one */
    trigger_list = trigger_list->next;
fprintf(log_file, "removed first one\n");
    free(trigger);
    return;
  }

  for (t = trigger_list ; t->next ; t = t->next) {
    if (t->next == trigger) {
      t->next = t->next->next;
fprintf(log_file, "removed internal one\n");
      free(trigger);
      return;
    }
  }

  return;
}


/*
 * Verify that the command read matches the next one we saved
 */
static boolean
ti103_cmd_verify (char house, int unit, func_value func) {

  if (tty == -1) {
    printf("Trying to verify command %c%02d %s at idx=%d\n",
	   house2name(house), unit, func2name(func), ti103_cmds_verify);
  }

  /*
   * If we have caught up with written commands,
   * there's nothing to verify
   */
  if (ti103_cmds_verify == ti103_cmds_write) {
    return FALSE;
  }

  if (ti103_cmds[ti103_cmds_verify].house == house &&
      ti103_cmds[ti103_cmds_verify].unit == unit &&
      ti103_cmds[ti103_cmds_verify].func == func) {
    if (tty == -1) {
      printf("Verified command %c%02d %s at idx=%d\n",
	     house2name(house), unit, func2name(func), ti103_cmds_verify);
    }

    ti103_cmds_verify++;
    if (ti103_cmds_verify >= MAX_OUTSTANDING_CMDS) {
      ti103_cmds_verify = 0;	/* loop around */
    }
    return TRUE;
  }

  return FALSE;
}


static void
event_log (int house, int unit, func_value function) {
  time_t		t;
  static int		last_house;
  static int		last_unit;
  static func_value	last_func;

  if (!events_file || *events_file == '\0') {
    return;
  }

  if (!events_fd) {
    events_fd = fopen(events_file, "a");
    if (!events_fd) {
      fprintf(log_file, "Error opening events file '%s'\n",
	      events_file);
      return;
    }
  }

  if (house == last_house && unit == last_unit &&
      function == last_func) {
    return;
  }

  /*
   * Log the event with a timestamp
   */
  time(&t);
  fprintf(events_fd, "Device %s (%c%0d) %s %s", 
	  x10_description(house, unit), house2name(house), unit,
	  func2name(function), ctime(&t));
  fflush(events_fd);

  last_house = house;
  last_unit = unit;
  last_func = function;
}


static boolean
parse_ti103_output (void) {
  char		*cptr, *cptr2, *endptr;
  static int	house;
  func_value	function;
  state_value	old_state, new_state;
  x10_desc	*desc;
  x10_trigger	*trigger;
  int		did_trigger=0;
  
  if (ti103_output_ptr == ti103_output) {
    return FALSE;
  }

  /*
   * See if we have a complete response first
   */
  for (cptr = ti103_output ; *cptr && *cptr != '#' ; cptr++) ;
  if (*cptr != '#') {
    /* Need to read more */
    return FALSE;
  } else {
    cptr -= 2;
    if (cptr < ti103_output) {
      ti103_output_ptr = ti103_output;
      return FALSE;
    }
    *cptr = '\0';
  }

  /*
   * Skip over the intro bytes
   */
  cptr = ti103_output;
  if (strncmp(cptr, TI103_REPLY_GOOD, sizeof(TI103_REPLY_GOOD)-1) == 0) {
    cptr += sizeof(TI103_REPLY_GOOD)-1;
  }

  if (strlen(cptr) > 0) {
    fprintf(log_file, "Parsing Buffer '%s'\n", cptr);
  }

  ti103_read_waiting = FALSE;	/* no longer waiting for input */

  /* Parse the string */
  while ( *cptr  ) {
    
    /* Skip spaces */
    if (*cptr == ' ') {
      cptr++;
      continue;
    }

    /* Found a house code */
    if (*cptr >= 'A' && *cptr <= 'P') {
      house = name2house(*cptr);	/* save house code */
      /*      printf("House code = %c\n", *cptr); */
      cptr++;
    }

    /* Found a digit or a function name */
    if (isdigit(*cptr)) {
      house_unit[house] = atoi(cptr);	/* save unit code */
      /*      printf("Unit code = %i\n", house_unit[house]); */
      while (isdigit(*cptr)) cptr++;
    } else if ((function = parse_ti103_function(&new_state, &cptr))
	       != no_function &&
	       house_unit[house] != 0) {
      fprintf(log_file, "Device %c %i %s\n", house2name(house),
	      house_unit[house], func2name(function));
      event_log(house, house_unit[house], function);
      ti103_cmd_verify(house, house_unit[house], function);
      if (function == hail_ack) {
	  (void)function_trigger(house, house_unit[house], function, NONE);
	  (void)all_trigger(house, house_unit[house], function);
	  did_trigger = 1;
      } else if ((new_state == state_on || new_state == state_off)) {
	  trigger = find_trigger(house, house_unit[house], function, NONE);
	  old_state = states[house][house_unit[house]];
	  states[house][house_unit[house]] = new_state;
	  if (old_state != new_state) {
	      fprintf(log_file, "  (Device %c%d ",
		      house2name(house), house_unit[house]);
	      desc = find_desc(house, house_unit[house]);
	      if (desc) {
		  fprintf(log_file, "(%s) ", desc->description);
	      }
	      fprintf(log_file, "changed state from %s to %s)\n",
		      state2name(old_state), state2name(new_state));
	  }
	  if ((trigger = find_trigger(house, house_unit[house], function, NEVER)) != NULL) {
	    did_trigger = 1;	/* pretend we did it */
	  } else if ((old_state != new_state) &&
		(trigger = find_trigger(house, house_unit[house], function, TRANSITION))) {
	    /* matching "trigger ..." */
	      (void)perform_trigger(trigger, house, house_unit[house], function);
	      did_trigger = 1;
	  } else if ((trigger = find_trigger(house, house_unit[house], function, ALWAYS)) != NULL) {
	    /* matching "always ..." */
	      (void)perform_trigger(trigger, house, house_unit[house], function);
	      did_trigger = 1;
	  }
	  if (!did_trigger) {
	    /* else, matching "always ..." */
	    (void)all_trigger(house, house_unit[house], function);
	  }
      }
      house_unit[house] = 0;	/* read complete house/unit/command */
      house = 0;
      fflush(log_file);
    } else {
      /* skip over a nonsense character */
      cptr++;
    }
  }

  /* update parsing to here */
  memmove(ti103_output, cptr, ti103_output_ptr - cptr);
  ti103_output_ptr = ti103_output;

  fflush(log_file);
  return TRUE;
}


static void ti103_cmd_print(void) {
  int	i;

  /* caught up with verification - could be a problem */
  fprintf(log_file,
	  "*** ti103_cmds_write == %d\nti103_cmds_verify == %d ***\n",
	  ti103_cmds_write, ti103_cmds_verify);

  for(i=0; i<MAX_OUTSTANDING_CMDS; i++) {
    fprintf(log_file, "** ti103_cmds[%d]='%c%02d %s'\n", i,
	    house2name(ti103_cmds[i].house), ti103_cmds[i].unit,
	    func2name(ti103_cmds[i].func));
  }
  fflush(log_file);
  /*  system("/usr/bin/mail -s 'X10 verified cmds error' raj@mischievous.us"); */
}

/*
 * Save away TI103 commands which have been written to the device
 * so they can be verified later.
 */
static void
ti103_cmd_write (char house, int unit, func_value func) {

  ti103_cmds[ti103_cmds_write].house = house;
  ti103_cmds[ti103_cmds_write].unit = unit;
  ti103_cmds[ti103_cmds_write].func = func;

  if (tty == -1) {
    printf("Saving command %c%02d %s at idx=%d\n",
	   house, unit, func2name(func), ti103_cmds_write);
  }

  ti103_cmds_write++;
  if (ti103_cmds_write == ti103_cmds_verify) {
    ti103_cmd_print();
  }
  if (ti103_cmds_write >= MAX_OUTSTANDING_CMDS) {
    ti103_cmds_write = 0;	/* loop around */
  }
}


/*
 * TI103 checksum is the sum of all characters but only
 * using the two low order bytes
 */
static void
write_ti103_checksum (char *prefix, char *command, int size) {
  char	buffer[4];
  char	*cp;
  int	i;

  i = 0;
  for (cp = prefix; *cp; cp++) {
    i += *cp;
  }
  for (cp = command; cp < &command[size]; cp++) {
    i += *cp;
  }

  sprintf(buffer, "%2x#", i & 0xff);
  fprintf(log_file, "Checksum=0x%2x\n", i & 0xFF);
  write_ti103(buffer, 3);
}


static void
write_ti103_command (char house, int unit, func_value func) {
  char		command[80];
  int		size;
  
  size = snprintf(command, sizeof(command),
		  "%c%02d%c%02d %c%s%c%s",
		  house2name(house),unit,
		  house2name(house),unit,
		  house2name(house), func2name(func),
		  house2name(house), func2name(func));

  fprintf(log_file, "Writing command '%s'\n", command);
  fflush(log_file);

  write_ti103(TI103_COMMAND, sizeof(TI103_COMMAND)-1);
  write_ti103(command, size);
  write_ti103_checksum(TI103_COMMAND, command, size);
/*  write_ti103(TI103_CKSUM, sizeof(TI103_CKSUM)-1); */

  /*
   * Store this away as our last command written
   */
  ti103_cmd_write(house, unit, func);

  if (tty == -1) {
    strcpy(ti103_output, TI103_REPLY_GOOD);
    strcat(ti103_output, command);
    strcat(ti103_output, TI103_CKSUM);
    ti103_output_ptr = &ti103_output[strlen(ti103_output)];
    parse_ti103_output();
  }
}



func_value
state2func(state_value state) 
{
    switch ( state ) {
    case state_on:
	return on;
    case state_off:
	return off;
    default:
	return no_function;
    }
}
		


/* Set this to say we received additional resets while doing resets */
boolean reset_x10_again=FALSE;

/*
 * Perform a full reset of all devices which are "on" or "off" as long
 * as they're not the house_code and unit_code of the reset trigger itself.
 */
void
reset_x10 (int reset_house, int reset_unit) 
{
    static time_t	last_reset=0;
    static boolean	resetting=FALSE;
    time_t		t;
    int			house;
    int			unit;

    if (resetting) {
	reset_x10_again=TRUE;		/* do another one */
	return;				/* already running */
    }
    resetting = TRUE;			/* lock out others */

    fprintf(log_file, "Resetting all X10 devices to known state\n");
    for (house = name2house('A'); house <= name2house('P'); house++) {
	for (unit = 1; unit <= 16; unit++) {
	    if ((states[house][unit] == state_on ||
		 states[house][unit] == state_off) &&
		!(house == reset_house && unit == reset_unit)) {
		fprintf(log_file, "Resetting %c%d to %s\n",
			house2name(house), unit,
			state2name(states[house][unit]));
		write_ti103_command(house, unit,
				    state2func(states[house][unit]));
	    }
	}
    }

    resetting = FALSE;
}




void
perform_trigger (x10_trigger *trigger, int house, int unit, func_value func) {
  char		buffer[256];

  /*
   * If the command is a zero length string, then it's a reset-all
   */
  if (trigger->command == NULL) {
      fprintf(log_file, "Requesting reset\n");
      reset_x10(house, unit);
      if (reset_x10_again) {
	  reset_x10_again = FALSE;
	  reset_x10(house, unit);
      }
      reset_x10_again = FALSE;

      fflush(log_file);
      return;
  }
  
  /*
   * Perform the command
   */
  fprintf(log_file, "%s: %c%d%s> %s\n",
	  (trigger->when == TRANSITION) ? "Trigger" : "Always",
	  house2name(house), unit, func2name(func),
	  trigger->command);
  strcpy(buffer, trigger->command);
  strcat(buffer, " </dev/null >/dev/null");
  fprintf(log_file, "returns %d\n", system(buffer));
}

/*
 * Perform the function associated with the house code and unit number.
 */
static boolean
function_trigger (int house, int unit, func_value func, when_t when) {
  x10_trigger	*trigger;
  char		*cmd;

  trigger = find_trigger(house, unit, func, when);
  if (!trigger)
    return FALSE;

  perform_trigger(trigger, house, unit, func);

  return TRUE;
}


static void
all_trigger (int house, int unit, func_value func) {
  x10_trigger	*trigger;
  char		*cmd;
  static char	buffer[256];

  trigger = find_trigger(0, 0, no_function, NONE);
  if (!trigger)
    return;

  substitute_characters(buffer, sizeof(buffer), trigger->command,
			house, unit, func);

  fprintf(log_file, "Trigger: %c%d%s> %s\n",
	  house2name(house), unit, func2name(func),
	  buffer);
  // strcpy(buffer, buffer);
  strcat(buffer, " </dev/null >/dev/null");
  fprintf(log_file, "returns %d\n", system(buffer));
}


static char *
skip_spaces (char *cp) {

  while (*cp && isspace(*cp)) cp++;

  return cp;
}


static char *
skip_until_space (char *cp) {

  while (*cp && !isspace(*cp)) cp++;

  return cp;
}


static char *
skip_digits (char *cp) {

  while (*cp && isdigit(*cp)) cp++;

  return cp;
}


/*
 * Parse a house and unit code
 * Side-effects:
 *	sets *ahouse to the house code number
 *	sets *aunit to the unit number
 *	updates *cptr to pointer after parsed items
 * Considerations:
 *	takes either upper or lower case
 * Returns TRUE only if both are parsed correctly
 */
static boolean
parse_house_unit (char **cptr, char *ahouse, int *aunit) {
  char	house;
  int	unit;
  char 	*cp;
  
  cp = *cptr;
  if ((*cp >= 'A' && *cp <= 'P') ||
      (*cp >= 'a' && *cp <= 'p')) {
    house = name2house(*cp);
    cp++;

    cp = skip_spaces(cp);
    if (isdigit(*cp)) {
      unit = atoi(cp);
      while (isdigit(*cp)) cp++;

      *ahouse = house;
      *aunit = unit;
      *cptr = cp;		/* update our pointer */
      return TRUE;		/* got both */
    }

    return FALSE;		/* incomplete */
  }

  return FALSE;			/* incomplete */
}


void
print_ti103_buffer(void) {
  time_t	t;

  if (ti103_output_ptr == ti103_output) {
    return;
  }

  if (strcmp(ti103_output, "4B#") == 0)
    return;

  time(&t);

  if (strcmp(ti103_output, TI103_REPLY_NULL) != 0) {
    fprintf(log_file, "%s: %s", ti103_output, ctime(&t));
    fflush(log_file);
  }
}


void
reset_ti103_buffer(void) {

  ti103_output_ptr = ti103_output;
}

  
/*
 * Read a result from the TI103
 */
void
read_ti103(boolean block) {
  char	buffer[128];
  int	ret;
  char	*cptr, *start;
  boolean	found_end, found_start;

  found_end = FALSE;
  found_start = FALSE;

  while (!found_end) {
    ret = read(tty, buffer, sizeof(buffer));
    if (ret == -1 && !found_start && !block) {
      return;
    } else if (ret == -1) {
      sleep(1);
      continue;
    }
    buffer[ret] = '\0';
    if (strcmp(buffer, TI103_REPLY_NULL) != 0) {
      fprintf(log_file, "Read TI103: (%d bytes)'%s'\n", ret, buffer);
    }
    
    /*
     * Append this to our TI103 output buffer
     */
    for (cptr = buffer ; *cptr ; cptr++) {
      if (strcmp(cptr, "$<") == 0) {
	found_start = TRUE;
      }

      if (ti103_output_ptr >= &ti103_output[sizeof(ti103_output)]) {
	fprintf(log_file, "! Ran out of TI103 buffer space\n");
	return;
      }
      *ti103_output_ptr++ = *cptr;
      *ti103_output_ptr = '\0';
      if (*cptr == '#') {
	found_end = TRUE;
	print_ti103_buffer();
	parse_ti103_output();
	
	/* Re-init to start a new command */
	found_start = FALSE;
	found_end = FALSE;
      }
    }
  }

  /*
   * Since we have a complete command output, parse it
   */
  print_ti103_buffer();
  parse_ti103_output();
}


static void
get_ti103_status (int sig) {

  if (tty == -1)
    return;

  if (verbose)
    fprintf(log_file, "Reading status\n");
  write_ti103(TI103_READ_BUFFER, sizeof(TI103_READ_BUFFER)-1);
}


static fd_set *
check_input (void) {
  static fd_set		readfds, zerofds;
  int rfds;
  int			nfds;
  int			ret;
  struct timeval	timeout;
  int			temp_tty;

  FD_ZERO(&readfds);
  FD_ZERO(&zerofds);
  FD_SET(STDIN_FILENO, &readfds);
  if (tty != -1) {
    FD_SET(tty, &readfds);
    nfds = tty + 1;		/* number fd bits set */
  } else {
    nfds = 2 + 1;		/* stderr + 1 */
  }
  
  /*
   * Enter select - will be interrupted by:
   * 1) new input from stdin
   * 2) new input from ti103
   * 3) the alarm which is set
   */
  bzero(&timeout, sizeof(timeout));
  timeout.tv_sec = 2;
  ret = select(nfds, &readfds, NULL, NULL, &timeout);
  /*  printf("ret=%d\n", ret); */
  if (ret <= 0) {
    /* Probably our alarm */
    return &zerofds;
  }

  return &readfds;
}


static void
print_status (FILE *fd,
	      char *output_file,
	      boolean desc) {
  int		i, j;
  FILE		*file;
  boolean	got_file;
  
  got_file = FALSE;
  if (fd) {			/* use output FD first */
    file = fd;
  } else if (output_file && *output_file) { /* filename 2nd */
    file = fopen(output_file, "a");
    if (file) {
      got_file = TRUE;
    }
  }

  if (!file) {			/* of log_file last */
    file = log_file;
  }

  for (i = 1 ; i <= 16 ; i++) {
    for (j = 1 ; j <= 16 ; j++) {
      if (states[i][j] != undefined) {
	if (desc) {
	  fprintf(file, "%s %-4s\n",
		  x10_description(i, j),
		  state2name(states[i][j]));
	} else {
	  fprintf(file, "%c %d %-4s\n",
		  house2name(i), j,
		  state2name(states[i][j]));
	}
      }
    }
  }
  fflush(file);

  if (got_file) {
    fclose(file);
  }
}


static x10_trigger *
new_trigger (int house, int unit, func_value func, char *cp) {
  x10_trigger	*trigger;

  trigger = malloc(sizeof(*trigger));
  if (!trigger)
    return NULL;

  trigger->house = house;
  trigger->unit = unit;
  trigger->function = func;
  trigger->command = NULL;
  if (cp) {
      trigger->command = malloc(strlen(cp)+1);
      if (!trigger->command) {
	  free(trigger);
	  return NULL;
      }
      strcpy(trigger->command, cp);
  }

  return trigger;
}


static void
insert_trigger(int house, int unit, func_value func, when_t when, char * cp) {
  x10_trigger	*trigger;
  
  trigger = new_trigger(house, unit, func, cp);
  if (!trigger)
    return;

  /*
   * Insert into linked list
   */
  trigger->next = trigger_list;
  trigger_list = trigger;
  trigger->when = when;
}


static void
replace_trigger (x10_trigger *trigger, char *cmd) {

    if (trigger->command) {
	free(trigger->command);
    }
    
    if (cmd && (strlen(cmd) > 0)) {
	trigger->command = malloc(strlen(cmd)+1);
	strcpy(trigger->command, cmd);
    } else {			/* remove it instead */
	remove_trigger(trigger);
    }
}


static void
add_trigger(int house, int unit, func_value func, when_t when, char * cp) {
  x10_trigger	*trigger;

  trigger = find_trigger(house, unit, func, when);
  if (!trigger) {
    fprintf(log_file, "New ");
    insert_trigger(house, unit, func, when, cp);
  } else {
    fprintf(log_file, "Replace ");
    replace_trigger(trigger, cp);
  }
  if (house == 0) {
    fprintf(log_file, "Trigger all: %s\n", cp);
  } else {
    fprintf(log_file, "Trigger: %c%d%s: %s\n",
	    house2name(house), unit, func2name(func), cp);
  }
  fflush(log_file);

}


static void
print_triggers (FILE *file) {
  x10_trigger	*trigger;

  for (trigger = trigger_list ; trigger ; trigger = trigger->next) {
    //    if (trigger->function == no_function) {
    if (trigger->house == 0 && trigger->unit == 0) {
      if (trigger->when == ALWAYS) {
	fprintf(file, "always all %s\n", trigger->command);
      } else if (trigger->when == NEVER) {
	fprintf(file, "never all %s\n", trigger->command);
      } else {
	fprintf(file, "trigger all %s\n", trigger->command);
      }
    } else if (!trigger->command) {
      if (trigger->when == ALWAYS) {
	fprintf(file, "always %c%02d %-4s\n",
		house2name(trigger->house), trigger->unit,
		func2name(trigger->function));
      }	else if (trigger->when == NEVER) {
	fprintf(file, "never %c%02d %-4s\n",
		house2name(trigger->house), trigger->unit,
		func2name(trigger->function));
      } else {
	fprintf(file, "trigger %c%02d %-4s\n",
		house2name(trigger->house), trigger->unit,
		func2name(trigger->function));
      }
    } else {
      if (trigger->when == ALWAYS) {
	fprintf(file, "always %c%02d %-4s %s\n",
		house2name(trigger->house), trigger->unit,
		func2name(trigger->function), trigger->command);
      } else if (trigger->when == NEVER) {
	fprintf(file, "never %c%02d %-4s %s\n",
		house2name(trigger->house), trigger->unit,
		func2name(trigger->function), trigger->command);
      } else {
	fprintf(file, "trigger %c%02d %-4s %s\n",
		house2name(trigger->house), trigger->unit,
		func2name(trigger->function), trigger->command);
      }
    }
  }
  fflush(file);
}


static void
upcase_word (char *cptr) {
  char	*cp;

  for (cp = cptr ; *cp && !isspace(*cp) ; cp++)
    if (islower(*cp))
      *cp = toupper(*cp);
}


static void
dump_state (char *output_file) {
  FILE		*file;
  boolean	got_file;
  
  got_file = FALSE;
  if (output_file && *output_file) {
    file = fopen(output_file, "w");
    if (file) {
      got_file = TRUE;
    }
  }

  /*
   * If no output avail., use log_file
   */
  if (!got_file) {
    file = log_file;
  }

  /*
   * Print any device -> descriptions we know
   */
  print_descs(file);

  /*
   * Dump all device states
   */
//  print_status(file, NULL, FALSE);

  /*
   * Now, dump all triggers
   */
  print_triggers(file);

  /*
   * Any "events" command we have
   */
  if (events_file != NULL) {
    fprintf(file, "events %s\n", events_file);
  }

  /*
   * And our "commands" command
   */
  if (command_file != NULL) {
    fprintf(file, "commands %s\n", command_file);
  }

  fflush(file);

  if (got_file) {
    fclose(file);
  }
}


static void
dump_and_exit (int signum) {

  dump_state(command_file);
  fclose(log_file);
  fclose(stdin_file);
  fclose(events_fd);

  exit(0);
}


/*
 * Reset everything to the current known state
 */
static void parse_stdin(char *buffer);

static void
reset_state(void) {
  int		i, j;
  char		buffer[80];

  for (i = 1 ; i <= 16 ; i++) {
    for (j = 1 ; j <= 16 ; j++) {
      if (states[i][j] != undefined) {
	sprintf(buffer, "%c %d %-4s\n",
		  house2name(i), j,
		  state2name(states[i][j]));
	parse_stdin(buffer);
      }
    }
  }
}


static void
parse_stdin (char *buffer) {
  char		*cp;
  char		house;
  int		unit;
  state_value	state;
  func_value	func;
  
  cp = buffer;

  cp = skip_spaces(cp);
  upcase_word(cp);
  if (strncmp(cp, "DESCRIPTION", 2) == 0) {
    cp = skip_until_space(cp);
    cp = skip_spaces(cp);
    if (parse_house_unit(&cp, &house, &unit)) {
      /* description is rest of line */
      cp = skip_spaces(cp);
      add_desc(house, unit, cp);
      return;
    }
  } else if (strncmp(cp, "TRIGGER", 1) == 0) {
    /* Skip keyword - read house/unit */
    cp = skip_until_space(cp);
    cp = skip_spaces(cp);
    upcase_word(cp);
    if (strncmp(cp, "ALL", 2) == 0) {
      cp = skip_until_space(cp);
      cp = skip_spaces(cp);
      add_trigger(0, 0, no_function, TRANSITION, cp);
      return;
    } else if (parse_house_unit(&cp, &house, &unit)) {
	cp = skip_spaces(cp);

	upcase_word(cp);
	func = parse_ti103_function(&state, &cp);
	cp = skip_spaces(cp);

	/* command is the rest of the line */
	add_trigger(house, unit, func, TRANSITION, cp);
	return;
    }
  } else if (strncmp(cp, "ALWAYS", 1) == 0) {
    fprintf(log_file, "Read always\n");
    /* Skip keyword - read house/unit */
    cp = skip_until_space(cp);
    cp = skip_spaces(cp);
    upcase_word(cp);
    if (strncmp(cp, "ALL", 2) == 0) {
      fprintf(log_file, "Read always all\n");
      cp = skip_until_space(cp);
      cp = skip_spaces(cp);
      add_trigger(0, 0, no_function, ALWAYS, cp);
      return;
    } else if (parse_house_unit(&cp, &house, &unit)) {
      fprintf(log_file, "Read always house/unit\n");
	cp = skip_spaces(cp);

	upcase_word(cp);
	func = parse_ti103_function(&state, &cp);
	cp = skip_spaces(cp);

	/* command is the rest of the line */
	add_trigger(house, unit, func, ALWAYS, cp);
	return;
    }
  } if (strncmp(cp, "NEVER", 1) == 0) {
    fprintf(log_file, "Read never\n");
    /* Skip keyword - read house/unit */
    cp = skip_until_space(cp);
    cp = skip_spaces(cp);
    upcase_word(cp);
    if (strncmp(cp, "ALL", 2) == 0) {
      fprintf(log_file, "Read never all\n");
      cp = skip_until_space(cp);
      cp = skip_spaces(cp);
      add_trigger(0, 0, no_function, NEVER, cp);
      return;
    } else if (parse_house_unit(&cp, &house, &unit)) {
      fprintf(log_file, "Read never house/unit\n");
	cp = skip_spaces(cp);
	upcase_word(cp);
	func = parse_ti103_function(&state, &cp);
	cp = skip_spaces(cp);

	/* command is the rest of the line */
	add_trigger(house, unit, func, NEVER, cp);
	return;
    }
  } else if (strncmp(cp, "RESET", 3) == 0) {
    /* Skip keyword - read house/unit */
    cp = skip_until_space(cp);
    cp = skip_spaces(cp);
    upcase_word(cp);
    if (parse_house_unit(&cp, &house, &unit)) {
	cp = skip_spaces(cp);

	upcase_word(cp);
	func = parse_ti103_function(&state, &cp);
	cp = skip_spaces(cp);

	/* command is the rest of the line */
	add_trigger(house, unit, func, ALWAYS, NULL);
	return;
    }
  } else if (strncmp(cp, "DUMP", 4) == 0) {
    cp = skip_until_space(cp);
    cp = skip_spaces(cp);
    dump_state(cp);
    ti103_cmd_print();
    return;
  } else if (strncmp(cp, "STATUS", 6) == 0) {
    cp = skip_until_space(cp);
    cp = skip_spaces(cp);
    print_status(NULL, cp, TRUE);
    return;
  } else if (strncmp(cp, "COMMANDS", 3) == 0) {
    cp = skip_until_space(cp);
    cp = skip_spaces(cp);
    if (*cp == '\0') {
      free(command_file);
      command_file = NULL;
    } else {
      command_file = strdup(cp);
    }
    return;
  } else if (strncmp(cp, "EVENTS", 3) == 0) {
    cp = skip_until_space(cp);
    cp = skip_spaces(cp);
    if (*cp == '\0') {
      free(events_file);
      events_file = NULL;
    } else {
      events_file = strdup(cp);
    }
    return;
  } else if (strncmp(cp, "REINIT", 3) == 0) {
    cp = skip_until_space(cp);
    cp = skip_spaces(cp);
    reset_state();
    return;
  }

  /*
   * Just upcase everything now
   *
   * All commands after this point can be converted to total CAPS
   */
  for (cp = buffer ; *cp ; cp++)
    if (islower(*cp))
      *cp = toupper(*cp);

  for (cp = buffer ; *cp ; cp++) {
    if (*cp == ' ')
      continue;

    if (strncmp(cp, "ON", 2) == 0) {
      if (tty == -1) {
	  (void)function_trigger(name2house(house), unit, on, NONE);
	  (void)all_trigger(name2house(house), unit, on);
      }
      write_ti103_command(name2house(house), unit, on);
      cp += 1;			/* all except last char */
    } else if (strncmp(cp, "OFF", 3) == 0) {
      if (tty == -1) {
	  (void)function_trigger(name2house(house), unit, off, NONE);
	  (void)all_trigger(name2house(house), unit, off);
      }
      write_ti103_command(name2house(house), unit, off);
      cp += 2;			/* all except last char */
    } else if (strncmp(cp, "HAK", 3) == 0) {
      if (tty == -1) {
	  (void)function_trigger(name2house(house), unit, hail_ack, NONE);
	  (void)all_trigger(name2house(house), unit, hail_ack);
      }
      write_ti103_command(name2house(house), unit, hail_ack);
      cp += 2;			/* all except last char */
    } else if (strncmp(cp, "HRQ", 3) == 0) {
      if (tty == -1) {
	  (void)function_trigger(name2house(house), unit, hail_request, NONE);
	  (void)all_trigger(name2house(house), unit, hail_request);
      }
      write_ti103_command(name2house(house), unit, hail_request);
      cp += 2;			/* all except last char */
    } else if (strncmp(cp, "SRQ", 3) == 0) {
      if (tty == -1) {
	  (void)function_trigger(name2house(house), unit, status_request, NONE);
	  (void)all_trigger(name2house(house), unit, status_request);
      }
      write_ti103_command(name2house(house), unit, status_request);
      cp += 2;			/* all except last char */
    } else if (strncmp(cp, "DIM", 3) == 0) {
      if (tty == -1) {
	  (void)function_trigger(name2house(house), unit, dim, NONE);
	  (void)all_trigger(name2house(house), unit, dim);
      }
      write_ti103_command(name2house(house), unit, dim);
      cp += 2;			/* all except last char */
    } else if (strncmp(cp, "BGT", 3) == 0) {
      if (tty == -1) {
	  (void)function_trigger(name2house(house), unit, bright, NONE);
	  (void)all_trigger(name2house(house), unit, bright);
      }
      write_ti103_command(name2house(house), unit, bright);
      cp += 2;			/* all except last char */
    } else if (strncmp(cp, "ALN", 3) == 0) {
      if (tty == -1) {
	  (void)function_trigger(name2house(house), unit, all_lights_on, NONE);
	  (void)all_trigger(name2house(house), unit, all_lights_on);
      }
      write_ti103_command(name2house(house), unit, all_lights_on);
      cp += 2;			/* all except last char */
    } else if (strncmp(cp, "ALF", 3) == 0) {
      if (tty == -1) {
	  (void)function_trigger(name2house(house), unit, all_lights_off, NONE);
	  (void)all_trigger(name2house(house), unit, all_lights_off);
      }
      write_ti103_command(name2house(house), unit, all_lights_off);
      cp += 2;			/* all except last char */
    } else if (strncmp(cp, "UND", 3) == 0) {
      states[name2house(house)][unit] = undefined;
      cp += 2;			/* all except last char */
    } else if (*cp >= 'A' && *cp <= 'P') {
      house = *cp;
      continue;
    } else if (isdigit(*cp)) {
      unit = atoi(cp);
      cp = skip_digits(cp);
      cp--;			/* adjust for incr. in loop */
    }
  }
}



void
read_stdin (void) {
  char	buffer[128];
  int	buflen;
  
  if (fgets(buffer, sizeof(buffer), stdin_file) == NULL) {
    if (feof(stdin_file)) {
      fprintf(log_file, "Pipe input ended\n");
      clearerr(stdin_file);
    }
    return;
  }

  /* Remove the '\n' from the end */
  buflen = strlen(buffer);

  if (buffer[buflen-1] == '\n') {
    buffer[buflen-1] = '\0';
  }

  fprintf(log_file, "Read command: '%s'\n", buffer);
  fflush(log_file);

  parse_stdin(buffer);
}


static void
read_commands (char *com_file) {
  FILE	*com;
  char	buffer[128];
  int	buflen;

  com = fopen(com_file, "r");
  if (!com) {
    return;
  }

  while (fgets(buffer, sizeof(buffer), com) != NULL) {
    /* Remove the '\n' from the end */
    buflen = strlen(buffer);
    
    if (buffer[buflen-1] == '\n') {
      buffer[buflen-1] = '\0';
    }
    
    fprintf(log_file, "Read command from file : '%s'\n", buffer);
    fflush(log_file);
    
    parse_stdin(buffer);
  }

  fclose(com);
}


int
main (int argc, char **argv) {
  fd_set	*readfds;
  time_t	t, last_t;

  init();

  time(&t);
  fprintf(log_file, "*** Starting up - %s", ctime(&t));
  /*
   * Open the device
   */
  tty = init_ti103();

  setuid(70);			/* setuid to _www */

  get_ti103_status(0);

  if (argc > 1) {
    read_commands(argv[1]);
  }

  /*
   * Read buffer
   */
  time(&last_t);
  while (TRUE) {
    if (tty == -1) {
      /* Try opening it again */
      tty = init_ti103();
    }
    readfds = check_input();

    if (tty != -1 && FD_ISSET(tty, readfds)) {
      read_ti103(FALSE);
    }
    if (FD_ISSET(STDIN_FILENO, readfds)) {
      read_stdin();
    }

    time(&t);
    if (t - last_t > 2) {
      get_ti103_status(0);
      last_t = t;
    }
  }
}
