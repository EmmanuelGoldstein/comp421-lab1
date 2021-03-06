#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
#include <hardware.h>
#include <terminals.h>

/*
 * echo Buffer size
 */
#define SIZE_OF_ECHO_BUFFER 1024
#define SIZE_OF_INPUT_BUFFER 1024
#define FAILED -1
#define SUCCESS 0

/*
 * state indicating whether output loop is establish
 */
static int echoing[NUM_TERMINALS];
/*
 * Number of reader waiting
 */
static int waitingreaders[NUM_TERMINALS];
/*
 * Number of writer waiting
 */
static int waitingwriters[NUM_TERMINALS];
/*
 * state whether other reader is reading
 */
static int reading[NUM_TERMINALS];
/*
 * state whether other writer is writing
 */
static int writing[NUM_TERMINALS];
/*
 * number of line breaks in the input buffer
 */
static int linebreaks[NUM_TERMINALS];
/*
 * state whether output buffer is drained
 */
static int outputcompleted[NUM_TERMINALS];

/*
 * one termstat for each terminal
 */
static struct termstat stats_local[NUM_TERMINALS];
/*
 * A condition variable for each readTerminal to wait on, only one reader
 * at a time.
 */
static cond_id_t read[NUM_TERMINALS];
/*
 * A condition variable for each writeTerminal to wait on, only one writer
 * at a time.
 */
static cond_id_t write[NUM_TERMINALS];
/*
 * A condition variable for each readTerminal to wait on, only afer a line
 * terminated by new line charater can one reader return.
 */
static cond_id_t linecompleted[NUM_TERMINALS];
/*
 * A condition variable for each writeTerminal to wait on, only after the output
 * buffer is drained should one writer return.
 */
static cond_id_t outputempty[NUM_TERMINALS];
/*
 * character buffer data structure
 */
typedef struct {
    size_t in;
    size_t out;
    int count;
    int size;
    char* data;
} queue_t;

static char dequeue(queue_t *queue) {
    if (queue->count == 0) {
        return (char) 0;
    }
    char output = queue->data[queue->out];
    queue->data[queue->out] = (char) 0;
    queue->count--;
    queue->out = (queue->out + 1) % queue->size;
    return output;
}

static int enqueue(queue_t *queue, char input) {
    if (queue->count == queue->size) {
        return FAILED;
    }
    queue->data[queue->in] = input;
    queue->in = (queue->in + 1) % queue->size;
    queue->count++;
    return SUCCESS;
}

static char destack(queue_t *queue) {
    if (queue->count == 0) {
        return (char) 0;
    }
    queue->in = (queue->in - 1) % queue->size;
    char output = queue->data[queue->in];
    queue->data[queue->in] = (char) 0;
    queue->count--;
    return output;
}

static int enstack(queue_t *queue, char input) {
    if (queue->count == queue->size) {
        return FAILED;
    }
    queue->out = (queue->out - 1) % queue->size;
    queue->data[queue->out] = input;
    queue->count++;
    return SUCCESS;
}

/*
 * Void queue
 */
static queue_t voidBuffer = (queue_t){0,0,0,-1, NULL};

/*
 * One echo buffer for each terminal
 */
static queue_t echoBuffers[NUM_TERMINALS];

/*
 * One input buffer for each terminal
 */
static queue_t inputBuffers[NUM_TERMINALS];

/*
 * One output buffer for each terminal
 */
static queue_t outputBuffers[NUM_TERMINALS];

/*
 * A flag for special '\n' processing of output buffer
 */
static int specialMeet[NUM_TERMINALS];

static void
enqueue_echo(int term, char received)
{
    if (echoing[term] == FAILED)
    {
        //not in the write and transmit interrupt loop
        //send the first WriteDataRegister call
        if (received == '\r') {
            WriteDataRegister(term, '\r');
            stats_local[term].tty_out = stats_local[term].tty_out + 1;
            enqueue(&echoBuffers[term], '\n');
        } else if (received == '\n') {
            WriteDataRegister(term, '\r');
            stats_local[term].tty_out = stats_local[term].tty_out + 1;
            enqueue(&echoBuffers[term], '\n');
        } else if (received == '\b' || received == '\177') {
            WriteDataRegister(term, '\b');
            stats_local[term].tty_out = stats_local[term].tty_out + 1;
            enqueue(&echoBuffers[term], ' ');
            enqueue(&echoBuffers[term], '\b');
        } else {
            //any other character
            WriteDataRegister(term, received);
            stats_local[term].tty_out = stats_local[term].tty_out + 1;
        }
        echoing[term] = SUCCESS;
    } else {
        if (received == '\r') {
            enqueue(&echoBuffers[term], '\r');
            enqueue(&echoBuffers[term], '\n');
        } else if (received == '\n') {
            enqueue(&echoBuffers[term], '\r');
            enqueue(&echoBuffers[term], '\n');
        } else if (received == '\b' || received == '\177') {
            enqueue(&echoBuffers[term], '\b');
            enqueue(&echoBuffers[term], ' ');
            enqueue(&echoBuffers[term], '\b');
        } else {
            //any other character
            enqueue(&echoBuffers[term], received);
        }
    }
}

/*
 * enqueue input buffer
 */
static int
enqueue_input(int term, char received)
{
    (void) term;
    int inputstatus;
    //char process before enqueue
    if (received == '\r') {
        inputstatus = enqueue(&inputBuffers[term], '\n');
        if (inputstatus == SUCCESS) {
            //enqueue success
            linebreaks[term] = linebreaks[term] + 1;
            if (linebreaks[term] == 1) {
                //signal readers that the line is completed.
                CondSignal(linecompleted[term]);
            }
            return inputstatus;
        } else {
            //queue full, enqueue failed
            return inputstatus;
        }

    } else if (received == '\n') {
        inputstatus = enqueue(&inputBuffers[term], received);
        if (inputstatus == SUCCESS) {
            //enqueue success
            linebreaks[term] = linebreaks[term] + 1;
            if (linebreaks[term] == 1) {
                //signal readers that the line is completed.
                CondSignal(linecompleted[term]);
            }
            return inputstatus;
        } else {
            //queue full, enqueue failed
            return inputstatus;
        }
    } else if (received == '\b' || received == '\177') {
        //remove one character from input buffer
        if (inputBuffers[term].count > 0) {
            destack(&inputBuffers[term]);
            return SUCCESS;
        }
        return FAILED;
        //empty input buffer, do nothing
    } else {
        //any other charater
        inputstatus = enqueue(&inputBuffers[term], received);
        return inputstatus;
    }
}

/*
 * Require procedures for hardwares
 */
extern void
ReceiveInterrupt(int term)
{
    Declare_Monitor_Entry_Procedure();
    char received = ReadDataRegister(term);
    stats_local[term].tty_in = stats_local[term].tty_in + 1;
    //add received to input buffer
    int input_status = enqueue_input(term, received);
    //add received to echo buffer
    if (input_status == FAILED) {
        //when input buffer is full
        enqueue_echo(term, '\a');
    } else {
        enqueue_echo(term, received);
    }

}

extern void
TransmitInterrupt(int term)
{
    Declare_Monitor_Entry_Procedure();
    if (outputBuffers[term].count == 0 && outputBuffers[term].size != -1) {
        //immediately signal the waiting write terminal caller that write is
        //completed.
        outputcompleted[term] = SUCCESS;
        CondSignal(outputempty[term]);
    }
    if (echoBuffers[term].count > 0) {
        //first output echo buffer
        char received = dequeue(&echoBuffers[term]);
        WriteDataRegister(term, received);
        stats_local[term].tty_out = stats_local[term].tty_out + 1;
    } else if (outputBuffers[term].count > 0) {
        //output buffer
        char received = dequeue(&outputBuffers[term]);
        //process special character '\n'
        if (received == '\n' && specialMeet[term] == FAILED) {
            specialMeet[term] = SUCCESS;
            WriteDataRegister(term, '\r');
            stats_local[term].tty_out = stats_local[term].tty_out + 1;
            enstack(&outputBuffers[term], '\n');
        } else if (received == '\n' && specialMeet[term] == SUCCESS){
            WriteDataRegister(term, '\n');
            stats_local[term].tty_out = stats_local[term].tty_out + 1;
            specialMeet[term] = FAILED;
        } else {
            //normal character, write as is
            WriteDataRegister(term, received);
            stats_local[term].tty_out = stats_local[term].tty_out + 1;
        }
    } else {
        //nothing to output
        echoing[term] = FAILED;
    }
}

extern int
WriteTerminal(int term, char *buf, int buflen)
{
    Declare_Monitor_Entry_Procedure();
    if (buflen == 0) {
        //given buffer is empty, return immediately
        return 0;
    }
    waitingwriters[term] = waitingwriters[term] + 1;
    //aquire exclusion on write
    while (writing[term] == SUCCESS) {
        CondWait(write[term]);
    }
    waitingwriters[term] = waitingwriters[term] - 1;
    writing[term] = SUCCESS;
    //assign the given buffer to input buffer's slot
    outputBuffers[term] = (queue_t){0,0,buflen, buflen, buf};
    //check if output and transmit interrupt loop is running
    if (echoing[term] == FAILED) {
        //initiate the first WriteRegister
        char first = dequeue(&outputBuffers[term]);
        //process special character '\n'
        if (first == '\n') {
            specialMeet[term] = SUCCESS;
            WriteDataRegister(term, '\r');
            stats_local[term].tty_out = stats_local[term].tty_out + 1;
            enstack(&outputBuffers[term], '\n');
        } else {
            WriteDataRegister(term, first);
            stats_local[term].tty_out = stats_local[term].tty_out + 1;
        }
        echoing[term] = SUCCESS;
    }
    //wait for the output are consumed entirely
    while(outputcompleted[term] == FAILED) {
        CondWait(outputempty[term]);
    }
    outputBuffers[term] = voidBuffer;
    outputcompleted[term] = FAILED;
    writing[term] = FAILED;
    if (waitingwriters[term] > 0) {
        CondSignal(write[term]);
    }
    stats_local[term].user_in = stats_local[term].user_in + buflen;
    return buflen;
}

extern int
ReadTerminal(int term, char *buf, int buflen)
{
    Declare_Monitor_Entry_Procedure();
    //case buflen = 0, nothing to read
    if (buflen == 0) {
        return 0;
    }
    waitingreaders[term] = waitingreaders[term] + 1;
    while (reading[term] == SUCCESS){
        //Wait for other reader to complete
        CondWait(read[term]);
    }
    //Current reader is reading
    waitingreaders[term] = waitingreaders[term] - 1;
    reading[term] = SUCCESS;
    while (linebreaks[term] <= 0) {
        //wait for the line to complete
        CondWait(linecompleted[term]);
    }
    //read the line
    int count = 0;
    while (count < buflen) {
        char curr = dequeue(&inputBuffers[term]);
        buf[count] = curr;
        count++;
        if (curr == '\n') {
            linebreaks[term] = linebreaks[term] - 1;
            break;
        }
    }
    //completed reading
    reading[term] = FAILED;
    //wake up other readers
    if (waitingreaders[term] > 0) {
        CondSignal(read[term]);
    }
    stats_local[term].user_out = stats_local[term].user_out + count;
    return count;
}

extern int
InitTerminal(int term)
{
    Declare_Monitor_Entry_Procedure();
    stats_local[term] = (struct termstat){0, 0, 0, 0};
    return InitHardware(term);
}

extern int
TerminalDriverStatistics(struct termstat *stats)
{
    Declare_Monitor_Entry_Procedure();
    int i;
    for (i = 0; i < NUM_TERMINALS; i++) {

        stats[i].tty_in = stats_local[i].tty_in;
        stats[i].tty_out = stats_local[i].tty_out;
        stats[i].user_in = stats_local[i].user_in;
        stats[i].user_out = stats_local[i].user_out;
    }
    return 0;
}

extern int
InitTerminalDriver()
{
    Declare_Monitor_Entry_Procedure();
    int i;
    for (i = 0; i < NUM_TERMINALS; i++) {
        //Initilaize states
        echoing[i] = FAILED;
        waitingreaders[i] = 0;
        waitingwriters[i] = 0;
        reading[i] = FAILED;
        writing[i] = FAILED;
        linebreaks[i] = 0;
        outputcompleted[i] = FAILED;
        stats_local[i] = (struct termstat) {-1, -1, -1, -1};
        //Initialize condition variables
        read[i] = CondCreate();
        write[i] = CondCreate();
        linecompleted[i] = CondCreate();
        outputempty[i] = CondCreate();
        //Initialize buffers
        echoBuffers[i] = (queue_t){0, 0, 0, SIZE_OF_ECHO_BUFFER,
                                   malloc(sizeof(char)*SIZE_OF_ECHO_BUFFER)};
        inputBuffers[i] = (queue_t){0, 0, 0, SIZE_OF_INPUT_BUFFER,
                                    malloc(sizeof(char)*SIZE_OF_ECHO_BUFFER)};
        outputBuffers[i] = voidBuffer;
        specialMeet[i] = FAILED;
    }

    return 0;
}
