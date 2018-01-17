
/*
 * Port forwarding/logging server
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <netinet/in.h>
#include <netdb.h>
#include <getopt.h>

#define NUMREPLACE 50   // maximum number of replacements
#define MAXREPLACE 512  // maximum length of replacements

// global variables nicely grouped
struct {
    int srcPort;        // listening port
    int dstPort;        // destination port
    char server[256];   // server address
    char buffer[1024];  // temporary buffer for input
    char outbuf[1024];  // buffer for output
    int rawFlag;        // raw logging
    int stripFlag;      // strip logging
    int hexFlag;        // hex logging
    int autoValue;      // autoN logging
    int replaceCount;   // number of replacements
    char replaceText[NUMREPLACE][MAXREPLACE];   // text to replace
    char replaceWith[NUMREPLACE][MAXREPLACE];   // replace with

} globals;

// report error message & exit
void die( const char * errorMessage, ...) {
    fprintf( stderr, "Error: ");
    va_list args;
    va_start( args, errorMessage);
    vfprintf( stderr, errorMessage, args);
    fprintf( stderr, "\n");
    va_end( args);
    exit(-1);
}

// read a line of text from file descriptor into provided buffer, up to provided char limit
int readLineFromFd( int fd, char * buff, int max) {
    char * ptr = buff;
    int count = 0;
    int result = 1;
    
    while (1) {

        // try to read in the next character from fd, exit loop on failure
        if (read(fd, ptr, 1) < 1) {
            result = 0;
            break;
        }

        // character stored, now advance ptr and character count
        ptr ++;
        count++;

        // if last character read was a newline, exit loop
        if (*(ptr - 1) == '\n') break;

        // if the buffer capacity is reached, exit loop
        if (count >= max - 1) break;        
    }
    
    // rewind ptr to the last read character
    ptr --;

    // trim trailing spaces (including new lines, telnet's \r's)
    while (ptr > buff && isspace(*ptr)) {
        ptr--;
    }

    // terminate the string
    * (ptr + 1) = '\0';
    
    return result;
}

// write a string to file descriptor
int writeStrToFd( int fd, char * str) {
    return write( fd, str, strlen( str));
}

// formats a string using logging options
void formatString(char *output, char *input, int out) {
    
    // raw format
    if (globals.rawFlag) {
        if (out) 
            sprintf(output, "--> %s\n", input);
        else 
            sprintf(output, "<-- %s\n", input);
    }

    // strip format
    else if (globals.stripFlag) {
        char temp[sizeof(globals.buffer)];
        strcpy(temp, input);

        // replace non printable characters with .
        for (int i = 0; i < strlen(temp); i++) {
            if (!isprint(temp[i]))
                temp[i] = '.';
        }
        if (out) 
            sprintf(output, "--> %s\n", temp);
        else 
            sprintf(output, "<-- %s\n", temp);
    }

    // hex format
    else if (globals.hexFlag) {
        char temp[16*sizeof(globals.buffer)];

        // create initial string
        if (out)
            strcpy(temp, "--> 00000000  ");
        else
            strcpy(temp, "<-- 00000000  ");
        int index = 14;

        // iterate through input characters
        for (int i = 0; i < strlen(input); i++) {

            // get decimal value
            int cint = input[i];

            // get hex value
            char chex[2];
            sprintf(chex, "%02X", cint);
            temp[index++] = chex[0];
            temp[index++] = chex[1];
            temp[index++] = ' ';

            // add spaces between octets
            if ((i+1) % 8 == 0 && (i+1) % 16 != 0)
                temp[index++] = ' ';

            // handle new lines and ASCII display
            else if ((i+1) % 16 == 0 || i == strlen(input) - 1) {

                // ASCII display
                int k;
                int nchar = (i+1) % 16;
                if (nchar == 0) nchar = 16;
                char actualstr[24];
                sprintf(actualstr, "  |");
                int actualindex = 3;

                // get last read input characters                
                for (k = 0; k < nchar; k++)
                    actualstr[actualindex+k] = input[i-nchar+k+1];
                
                actualstr[actualindex+k] = '|';
                actualstr[actualindex+k+1] = '\0';

                // add to string, replacing non printable with .
                for (int l = 0; l < strlen(actualstr); l++) {
                    if (isprint(actualstr[l]))
                        temp[index++] = actualstr[l];
                    else
                        temp[index++] = '.';
                }

                // new lines after 16 characters or end of input
                if ((i+1) % 16 == 0) {
                    char newlinestr[15];
                    if (out)
                        sprintf(newlinestr, "\n--> %08X  ", (i+1));
                    else
                        sprintf(newlinestr, "\n<-- %08X  ", (i+1));
                    for (int j = 0; j < 15; j++)
                        temp[index++] = newlinestr[j];
                }
            }
        }

        // end string
        temp[index++] = '\n';
        temp[index] = '\0';
        strcpy(output, temp);
    }

    // autoN format
    else if (globals.autoValue > 0) {
        char temp[16*sizeof(globals.buffer)];
        if (out)
            strcpy(temp, "--> ");
        else
            strcpy(temp, "<-- ");
        int index = 4;
        
        // iterate over input characters
        for (int i = 0; i < strlen(input); i++) {
            int cint = input[i];

            // backslash
            if (cint == 92) {
                temp[index++] = '\\';
                temp[index++] = '\\';
            }

            // tab
            else if (cint == 9) {
                temp[index++] = '\\';
                temp[index++] = 't';
            }

            // newline
            else if (cint == 10) {
                temp[index++] = '\\';
                temp[index++] = 'n';
            }

            // carriage return
            else if (cint == 13) {
                temp[index++] = '\\';
                temp[index++] = 'r';
            }

            // printable values unchanged
            else if (cint >= 32 && cint <= 127) {
                temp[index++] = input[i];
            }

            // all other characters: \[hex value]
            else {
                char chex[3];
                sprintf(chex, "\\%02X", cint);
                for (int j = 0; j < 3; j++)
                    temp[index++] = chex[j];
            }

            // handle number of bytes to split by
            if ((i+1) % globals.autoValue == 0) {
                char newlinestr[6];
                if (out)
                    sprintf(newlinestr, "\n--> ");
                else
                    sprintf(newlinestr, "\n<-- ");
                for (int k = 0; k < 5; k++)
                    temp[index++] = newlinestr[k];               
            }
        }

        // end string
        temp[index++] = '\n';
        temp[index] = '\0';
        strcpy(output, temp);
    }

    // no logging options
    else {
        strcpy(output, "");
    }
}

// uses replace options to replace substrings of data
void replaceString(char *output, char *input) {

    char temp[4*sizeof(globals.buffer)];
    strcpy(temp, input);

    // iterate over all replacements
    for (int i = 0; i < globals.replaceCount; i++) {

        // break if text to replace is empty string
        if (strcmp(globals.replaceText[i], "") == 0) {
            break;
        }

        // otherwise do the replacement
        else {
            size_t nin = strlen(globals.replaceText[i]);
            size_t nout = strlen(globals.replaceWith[i]);
            const char *tmp = temp;
            char buf[1024] = { 0 };
            char *insptr = &buf[0];

            while (1) {
                const char *p = strstr(tmp, globals.replaceText[i]);

                // past last instance of text to replace, keep the rest
                if (p == NULL) {
                    strcpy(insptr, tmp);
                    break;
                }

                // copy before replace text
                memcpy(insptr, tmp, p - tmp);
                insptr += p - tmp;

                // copy string to replace with
                memcpy(insptr, globals.replaceWith[i], nout);
                insptr += nout;

                // set pointers
                tmp = p + nin;
            }

            // save string for this replacement
            strcpy(temp, buf);
        }
    }
    // output string with all replacements
    strcpy(output, temp);
}

// thread function for handling forwarding connections
void *connThread(void *arg) {

    int connSockFd = *(int *)arg;
    int destSockFd, n;
    struct sockaddr_in destaddr;
    struct hostent *server;

    // open destination socket
    destSockFd = socket(AF_INET, SOCK_STREAM, 0);

    if (destSockFd < 0) {
        printf("Destination socket failed\n");
        writeStrToFd(connSockFd, "Destination socket failed\n");
        return NULL;
    }

    // get destination host
    server = gethostbyname(globals.server);

    if (server == NULL) {
        printf("Destination server null\n");
        writeStrToFd(connSockFd, "Destination server null\n");
        return NULL;
    }

    // connect to destination server
    bzero((char *)&destaddr, sizeof(destaddr));
    destaddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, (char *)&destaddr.sin_addr.s_addr, server->h_length);
    destaddr.sin_port = htons(globals.dstPort);

    if (connect(destSockFd, (struct sockaddr *)&destaddr, sizeof(destaddr)) < 0) {
        printf("Connection to destination server failed\n");
        writeStrToFd(connSockFd, "Could not reach destination server\n");
        return NULL;
    }

    printf("Connected to destination server\n");

    // initialize variables for select()
    fd_set srcfds, destfds;
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    int srcret, destret;

    // main thread loop
    while (1) {

        // check if destination has data to read
        FD_ZERO(&destfds);
        FD_SET(destSockFd, &destfds);
        destret = select(destSockFd+1, &destfds, NULL, NULL, &tv);
        
        if (destret > 0) {
            // if there is data, read it and send to the source
            bzero(globals.buffer, sizeof(globals.buffer)-1);
            read(destSockFd, globals.buffer, sizeof(globals.buffer));
            globals.buffer[sizeof(globals.buffer)] = '\0';

            // apply replace options
            char replaced[4*sizeof(globals.buffer)];
            replaceString(replaced, globals.buffer);
            if (writeStrToFd(connSockFd, replaced) < 1)
                break;

            // apply logging options
            char formatted[16*sizeof(globals.buffer)];
            formatString(formatted, replaced, 0);
            printf(formatted);
        }

        // check if source has data to read
        int iof = -1;
        FD_ZERO(&srcfds);
        FD_SET(connSockFd, &srcfds);
        srcret = select(connSockFd+1, &srcfds, NULL, NULL, &tv);

        if (srcret > 0 && FD_ISSET(connSockFd, &srcfds)) {

            // if there is data, read it and send to the destination
            if ((iof = fcntl(connSockFd, F_GETFL, 0)) != -1)
                fcntl(connSockFd, F_SETFL, iof | O_NONBLOCK);

            bzero(globals.buffer, sizeof(globals.buffer)-1);
            read(connSockFd, globals.buffer, sizeof(globals.buffer));
            globals.buffer[sizeof(globals.buffer)] = '\0';

            // apply replace options
            char replaced[4*sizeof(globals.buffer)];
            replaceString(replaced, globals.buffer);
            if (writeStrToFd(destSockFd, replaced) < 1)
                break;

            // apply logging options
            char formatted[16*sizeof(globals.buffer)];
            formatString(formatted, replaced, 1);
            printf(formatted);

            if (iof != -1)
                fcntl(connSockFd, F_SETFL, iof);
        }
    }

    // close sockets
    close(destSockFd);
    close(connSockFd);
    printf("Connection terminated\n");
}

// main program function (entry point)
int main( int argc, char ** argv) {
    printf("Port forwarding/logging server 1.0\n");

    // initialize globals
    int c;
    globals.rawFlag = 0;
    globals.stripFlag = 0;
    globals.hexFlag = 0;
    globals.autoValue = 0;
    globals.replaceCount = 0;
    for (int i = 0; i < NUMREPLACE; i++) {
        strcpy(globals.replaceText[i], "");
        strcpy(globals.replaceWith[i], "");
    }

    // parse command line arguments and options
    while (1) {

        // use getopt to parse options
        static struct option long_options[] =
        {
            {"raw",     no_argument,       0, 'w'},
            {"strip",   no_argument,       0, 's'},
            {"hex",     no_argument,       0, 'h'},
            {"auto",    required_argument, 0, 'a'},
            {"replace", required_argument, 0, 'r'},
            {0, 0, 0, 0}
        };

        int option_index = 0;
        c = getopt_long (argc, argv, "wsha:r:",long_options, &option_index);

        // break if end of options
        if (c == -1) break;

        // apply options
        switch (c) {
            // raw
            case 'w':
                globals.rawFlag = 1;
                break;

            // strip
            case 's':
                globals.stripFlag = 1;
                break;

            // hex
            case 'h':
                globals.hexFlag = 1;
                break;

            // autoN
            case 'a':
                globals.autoValue = atoi(optarg);
                break;

            // replace
            case 'r':
                strcpy(globals.replaceText[globals.replaceCount], optarg);
                if (optind < argc && *argv[optind] != '-') {
                    strcpy(globals.replaceWith[globals.replaceCount], argv[optind]);
                    optind++;
                }
                else {
                    die("Replace option usage: --replace [replaceText] [replaceWith]\n");
                }
                if (globals.replaceCount < NUMREPLACE)
                    globals.replaceCount++;
                break;

            // otherwise
            default:
                break;
        }
    }

    // check for multiple logging options
    int flags = globals.rawFlag + globals.stripFlag + globals.hexFlag;
    if (flags > 1 || (flags > 0 && globals.autoValue > 0)) {
        die("You have selected too many logging options");
    }

    // parse non-option arguments (srcPort, server, destPort)
    if (argc - optind != 3) die( "Usage: ./proxy [logOptions] [replaceOptions] srcPort server dstPort\n");
    char *end = NULL;
    globals.srcPort = strtol(argv[optind], &end, 10);
    if (*end != 0) die("Bad source port %s", argv[optind]);
    globals.dstPort = strtol(argv[optind+2], &end, 10);
    if (*end != 0) die("Bad destination port %s", argv[optind+2]);
    strcpy(globals.server, argv[optind+1]);

    // create a listening socket on the given source port
    struct sockaddr_in servaddr;
    int listenSockFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSockFd < 0) die("Socket() failed");
    bzero((char*) &servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htons(INADDR_ANY);
    servaddr.sin_port = htons(globals.srcPort);
    if (bind(listenSockFd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        die("Could not bind listening socket: %s", strerror(errno));

    // listen for a new connection
    if (listen(listenSockFd, 3) != 0)
        die( "Could not listen for incoming connections");

    printf("Listening for connections...\n");

    // main server loop
    while (1) {

        // accept a new connection
        int connSockFd = accept(listenSockFd, NULL, NULL);
        if (connSockFd < 0) {
            printf("Accept() failed: %s", strerror(errno));
            continue;
        }
        printf("\nAccepted a new connection\n");

        // create a thread to handle the connection
        pthread_t tid;
        pthread_create(&tid, NULL, connThread, &connSockFd);
    }

    // end of program
    pthread_exit(NULL);
    close(listenSockFd);
    return 0;
}


