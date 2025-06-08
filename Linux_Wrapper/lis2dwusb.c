#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <sys/time.h>

#define SERIAL_PORT "/dev/ttyACM0"
#define BAUDRATE B2000000
#define LINE_BUF_SIZE 128

volatile int running = 1;
void handle_sigint(int _) { running = 0; }


// Configure the serial port with the specified baud rate and settings
// Returns 0 on success, -1 on error
int configure_serial(int fd)
{
    struct termios tty;

    if (tcgetattr(fd, &tty) != 0)
        return -1;

    cfsetospeed(&tty, BAUDRATE);
    cfsetispeed(&tty, BAUDRATE);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_iflag = IGNPAR;
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 1;

    return tcsetattr(fd, TCSANOW, &tty);
}

// Print usage information and exit
void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-s file.csv] [-f freq]\n"
        "  -s file.csv   Save output to CSV file\n"
        "  -f freq       Sampling frequency (200–1600 Hz)\n",
        prog);
    exit(1);
}

// Main function
int main(int argc, char *argv[])
{
    const char *save_path = NULL;
    int freq = 0;

    // Parse arguments
    int opt;
    while ((opt = getopt(argc, argv, "s:f:")) != -1)
    {
        switch (opt)
        {
        case 's':  // Save path for CSV output
            save_path = optarg;
            break;
        case 'f':  // Frequency in Hz
            freq = atoi(optarg);
            break;
        case 'h':  // Help  
        default:
            usage(argv[0]);
        }
    }

    // Validate frequency
    if (freq != 0 && (freq < 200 || freq > 1600))
    {
        fprintf(stderr, "Frequency must be between 200 and 1600 Hz\n");
        return 1;
    }

    // Setup signal handler
    signal(SIGINT, handle_sigint);

    // Raw mode for instant 'Q' detection
    struct termios orig_termios, raw_termios;
    tcgetattr(STDIN_FILENO, &orig_termios);
    raw_termios = orig_termios;
    raw_termios.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw_termios);

    // Open and configure serial
    int fd = open(SERIAL_PORT, O_RDWR | O_NOCTTY);
    if (fd < 0) { perror("open serial"); return 2; }
    if (configure_serial(fd) != 0) { perror("configure serial"); close(fd); return 3; }
    FILE *serial = fdopen(fd, "r+");
    if (!serial) { perror("fdopen"); close(fd); return 4; }

    // Flush input and send frequency command
    tcflush(fd, TCIFLUSH);
    usleep(100000);
    if (freq > 0)
    {
        // Send frequency command to RP2040
        fprintf(serial, "F=%d\r\n", freq);
        fflush(serial);
    }

    // Wait for "time,x,y,z" header
    char line[LINE_BUF_SIZE];
    int line_pos = 0, got_header = 0;

    while (!got_header && running)
    {
        char c;
        if (read(fd, &c, 1) == 1)
        {
            if (c == '\n')
            {
                line[line_pos] = '\0';
                if (strncmp(line, "time,", 5) == 0)
                {
                    got_header = 1;
                    //fprintf(stderr, "[INFO] Got header: %s\n", line);
                    break;
                }
                line_pos = 0;
            }
            else if (line_pos < LINE_BUF_SIZE - 1)
            {
                line[line_pos++] = c;
            }
        }
    }

    // Check if we got the header
    if (!got_header)
    {
        fprintf(stderr, "No header from device. Is firmware running?\n");
        fclose(serial);
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
        return 5;
    }

    // Wde are connected and ready to read data
    fprintf(stderr, "Press Q to stop\n");

    FILE *csv = NULL;
    // If save path is provided, open CSV file
    if (save_path)
    {
        csv = fopen(save_path, "w");
        if (!csv)
        {
            perror("open CSV file");
            fclose(serial);
            tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
            return 6;
        }
        fprintf(csv, "time,x,y,z\n");
    }

    unsigned long samples = 0;
    struct timeval start, end;
    gettimeofday(&start, NULL);

    // Set stdin non-blocking
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

    while (running)
    {
        // Check for 'q' key without Enter
        char c;
        if (read(STDIN_FILENO, &c, 1) == 1 && (c == 'q' || c == 'Q'))
            break;

        // Read line from serial
        if (!fgets(line, sizeof(line), serial))
            continue;

        int xi, yi, zi;
        // Parse the line for x, y, z values
        if (sscanf(line, "%d,%d,%d", &xi, &yi, &zi) == 3)
        {
            // Convert to float and scale G
            float xf = xi * 0.001f;
            float yf = yi * 0.001f;
            float zf = zi * 0.001f;

             // Calculate elapsed time
            struct timeval now;
            gettimeofday(&now, NULL);
            double elapsed_sample = (now.tv_sec - start.tv_sec) + (now.tv_usec - start.tv_usec) / 1e6;


            // Print to CSV or stdout
            if (csv)
                fprintf(csv, "%.3f,%.3f,%.3f,%.3f\n", elapsed_sample, xf, yf, zf);
            else
                printf("time = %.3f, x = %.3f, y = %.3f, z = %.3f\n", elapsed_sample, xf, yf, zf);

            samples++;
        }
    }

    // Stop and clean up
    fprintf(serial, "Q\r\n");
    fflush(serial);

    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1e6;

    if (csv) fclose(csv);
    fclose(serial);
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios); // Restore terminal

    fprintf(stderr, "Captured %lu samples in %.2f s = %.1f Hz\n",
            samples, elapsed, samples / elapsed);

    return 0;
}
