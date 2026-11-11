// Using server.cpp as skeleton and TCPLib.h from sample
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <errno.h>
#include "TCPLib.h"

#include <string>
#include <sstream>
#include <iostream>

using namespace std;

// GLOBAL VARIABLES
const size_t COMMAND_BUFFER_SIZE = 1024;
const size_t FILE_BUFFER_SIZE = 4096;
const size_t MAX_COMMAND_LENGTH = 4096;
const int LISTEN_BACKLOG = 10;
const int DATA_TIMEOUT_SECONDS = 30;
const int INVALID_DATA_PORT = -1;

// Precondition: Need a command from the user and a valid connfd that identifies the client's control connection
// Postcondition: Processes the requestewd command sends a status responcse and updates session state
void processCommand(const string& command, int connfd, bool& running, int& dataPort, const sockaddr_in& clientAddr);
// Precondition: Requires connfd to have a valid connected socket and message to send back to the client
// Postcondition: If returns true, successfully sends message to client
bool sendResponse(int connfd, const string& message);
// Precondition: The server can execute ls in its current working directory
// Postcondition: Fills listing and returns true on success, otherwise returns false
bool getDirectoryListing(string& listing);
// Precondition: The client address and listening data port are valid
// Postcondition: Returns a connected socket, or -1 if the connection fails
int connectToClientDataPort(const sockaddr_in& clientAddr, int dataPort);


int main(int argc, char** argv)
{
    int connfd;
    int listenfd;

    char* endptr;

    long port;

    // Use argc to check for how many command lines arguments were passed
    // Use argv to keep arguments as strings
    // Check to see if port number was provided, otherwise return unsuccessfully
    if (argc != 2)
    {
        cout << "Usage: " << argv[0] << " <PORTNUMBER>\n";
        return 1;
    }

    // Read and validate given port, Convert from string into number
    port = strtol(argv[1], &endptr, 10);

    // Return unsuccessful if port number is invalid
    if (*endptr != '\0' || port < 1 || port > 65535)
    {
        cout << "Invalid port number. Use 1-65535.\n";
        return 1;
    }

    // Display port
    cout << "FTP server configured for port " << port << '\n';

    // Create TCP socket identifer for OS (listening file descriptor)
    listenfd = socket(AF_INET, SOCK_STREAM, 0);

    // Check if TCP socket was created successfully
    if (listenfd < 0)
    {
        perror("socket");
        return 1;
    }

    // To be used to store IPv4 address and port number
    struct sockaddr_in serverAddress;

    // Set address structure to 0
    memset(&serverAddress, 0, sizeof(serverAddress));

    // Specify IPv4
    serverAddress.sin_family = AF_INET;
    // Convert 32-bit value into "network byte order"
    // INADDR_ANY = Listen to ALL available IPv4 interfaces
    serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    // Convert 16-bit port into "network byte order"
    serverAddress.sin_port = htons(static_cast<uint16_t>(port));

    // Associate TCP socket with port number
    // Will fail if another application is using port
    if (bind(listenfd, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0)
    {
        perror("bind");
        close(listenfd);
        return 1;
    }

    // The specified socket will now start accepting incoming TCP connections
    // Does NOT wait for connection, prepares it to accept connections
    if (listen(listenfd, LISTEN_BACKLOG) < 0)
    {
        perror("listen");
        close(listenfd);
        return 1;
    }

    cout << "FTP server listening on port " << port << '\n';

    // Use this to store information about incoming client(IP address and port)
    struct sockaddr_in clientAddr;
    // Specifies size of the client's address structure
    socklen_t clientLen;

    // Loop continuously. Can only accept one connection at a time
    while (true)
    {
        cout << "Waiting for a client to connect...\n";

        clientLen = sizeof(clientAddr);

        // Will idle here until we get a connection
        connfd = accept(listenfd, (struct sockaddr*)&clientAddr, &clientLen);

        // Found connection, but check connfd for errors, otherwise continue
        if (connfd < 0)
        {
            perror("accept");
            continue;
        }

        cout << "Client connected successfully!\n";
        
        // Create buffer for commands
        char buffer[COMMAND_BUFFER_SIZE];
        string pendingData;
        int dataPort = INVALID_DATA_PORT;
        bool running = true;

        // Continue until client disconnects
        // We read the entire input until we reach newline character ('\n')
        while (running)
        {
            memset(buffer, 0, sizeof(buffer));

            // Contains number of bytes received from socket
            ssize_t bytesReceived = recv(connfd, buffer, sizeof(buffer), 0);

            // An error has occurred
            if (bytesReceived < 0)
            {
                if (errno == EINTR)
                    continue;
                perror("recv");
                break;
            }

            // Client closes sending side
            if (bytesReceived == 0)
            {
                cout << "Client disconnected.\n";
                break;
            }

            // Store bytes before being fully processed
            pendingData.append(buffer, bytesReceived);
            
            size_t newlinePos;

            // Search for newline character, put its position into newlinePos
            while (running && (newlinePos = pendingData.find('\n')) != string::npos)
            {
                // Get the characters before newlinePos, write them into command
                string command = pendingData.substr(0, newlinePos);

                // Reject excessively long commands before processing them.
                if (command.size() > MAX_COMMAND_LENGTH || command.find('\0') != string::npos)
                {
                    sendResponse(connfd, "500 INVALID OR OVERSIZED COMMAND\n");
                    running = false;
                    break;
                }

                // Clear pendingData
                pendingData.erase(0, newlinePos + 1);

                cout << "Received complete command: " << command << '\n';
                processCommand(command, connfd, running, dataPort, clientAddr);
            }

            // Limit incomplete commands that never contain a newline.
            if (running && pendingData.size() > MAX_COMMAND_LENGTH)
            {
                sendResponse(connfd, "500 COMMAND TOO LONG\n");
                running = false;
                break;
            }

        }
        // Client Disconnects, close connection
        close(connfd);
    }

    return 0;
}

// Precondition: Need a command from the user and a valid connfd that identifies the client's control connection
// Postcondition: Processes the requestewd command sends a status responcse and updates session state
void processCommand(const string& command, int connfd, bool& running, int& dataPort, const sockaddr_in& clientAddr)
{
    istringstream stream(command);

    string firstLine;
    string fileName;
    string badInput;
    string portText;
    string response;
    string ready;
    bool transferSuccess;

    char* endptr;
    long requestedPort;

    stream >> firstLine;

    // Recieve and check client's temporary data port number if action is PORT
    if (firstLine == "PORT")
    {

        if (!(stream >> portText) || (stream >> badInput))
        {
            response = "501 INVALID PORT ARGUMENTS\n";
            if (!sendResponse(connfd, response))
            {
                running = false;
            }
            return;
        }

        requestedPort = strtol(portText.c_str(), &endptr, 10);

        if (portText.empty() || *endptr != '\0' ||
            requestedPort < 1 || requestedPort > 65535)
        {
            response = "501 INVALID PORT NUMBER\n";
            if (!sendResponse(connfd, response))
                running = false;
            return;
        }

        dataPort = static_cast<int>(requestedPort);

        cout << "Client data port set to " << dataPort << '\n';

        response = "200 PORT ACCEPTED\n";
        if (!sendResponse(connfd, response))
            running = false;

        return;
    }
    
    else if (firstLine == "get")
    {
        // Check that a filename was provided
        if (!(stream >> fileName) || (stream >> badInput))
        {
            response = "501 INVALID ARGUMENTS\n";
            if (!sendResponse(connfd, response))
                running = false;
            return;
        }

        // Only allow simple filenames in the server's working directory
        if (fileName.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "0123456789._-") != string::npos ||
            fileName == "." || fileName == "..")
        {
            response = "501 INVALID FILENAME\n";
            if (!sendResponse(connfd, response))
                running = false;
            return;
        }

        // Check that the client supplied a data port
        if (dataPort == INVALID_DATA_PORT)
        {
            response = "425 DATA PORT NOT SET\n";
            if (!sendResponse(connfd, response))
                running = false;
            return;
        }

        // Open the requested file
        FILE* fp = fopen(fileName.c_str(), "rb");

        if (fp == nullptr)
        {
            response = "550 FILE UNAVAILABLE\n";
            if (!sendResponse(connfd, response))
                running = false;

            dataPort = INVALID_DATA_PORT;
            return;
        }

        // Connect to the client's temporary data port
        int datafd = connectToClientDataPort(clientAddr, dataPort);

        dataPort = INVALID_DATA_PORT;

        if (datafd < 0)
        {
            fclose(fp);

            response = "425 DATA CONNECTION FAILED\n";
            if (!sendResponse(connfd, response))
                running = false;
            return;
        }

        // Tell the client that the transfer is beginning
        string ready = "150 READY\n";

        if (!sendResponse(connfd, ready))
        {
            fclose(fp);
            close(datafd);
            running = false;
            return;
        }

        // Read and transmit the file in chunks
        char fileBuffer[FILE_BUFFER_SIZE];
        size_t bytesRead;
        size_t totalToClient = 0;
        transferSuccess = true;

        while ((bytesRead = fread(fileBuffer, 1,
            sizeof(fileBuffer), fp)) > 0)
        {
            size_t totalSent = 0;

            while (totalSent < bytesRead)
            {
                ssize_t bytesSent = send(
                    datafd,
                    fileBuffer + totalSent,
                    bytesRead - totalSent,
                    MSG_NOSIGNAL
                );

                if (bytesSent <= 0)
                {
                    perror("File transfer");
                    transferSuccess = false;
                    break;
                }

                totalSent += bytesSent;
                totalToClient += bytesSent;
            }

            if (!transferSuccess)
            {
                break;
            }
        }

        // Check whether reading the file failed.
        if (ferror(fp))
        {
            transferSuccess = false;
        }

        // Close the file and temporary data connection.
        if (fclose(fp) != 0)
        {
            transferSuccess = false;
        }

        close(datafd);

        // Report the result over the control connection.
        if (transferSuccess)
        {
            cout << "GET successful: " << fileName << " (" << totalToClient << " bytes)\n";

            response = "226 TRANSFER COMPLETE\n";
            if (!sendResponse(connfd, response))
                running = false;
        }
        else
        {
            cout << "GET failed: " << fileName << '\n';

            response = "426 TRANSFER FAILED\n";
            if (!sendResponse(connfd, response))
                running = false;
        }

        return;
    }
    else if (firstLine == "put")
    {
        char totalFileBuffer[FILE_BUFFER_SIZE];
        size_t totalReceived = 0;
        transferSuccess = true;

        // Validate the filename.
        if (!(stream >> fileName) || (stream >> badInput))
        {
            response = "501 INVALID ARGUMENTS\n";
            if (!sendResponse(connfd, response))
                running = false;
            return;
        }

        // Only allow simple filenames.
        if (fileName.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "0123456789._-") != string::npos ||
            fileName == "." || fileName == "..")
        {
            response = "501 INVALID FILENAME\n";
            if (!sendResponse(connfd, response))
                running = false;
            return;
        }

        // Check whether a data port was provided.
        if (dataPort == INVALID_DATA_PORT)
        {
            response = "425 DATA PORT NOT SET\n";
            if (!sendResponse(connfd, response))
                running = false;
            return;
        }

        // Establish the temporary data connection.
        int datafd = connectToClientDataPort(clientAddr, dataPort);

        dataPort = INVALID_DATA_PORT;

        if (datafd < 0)
        {
            response = "425 DATA CONNECTION FAILED\n";
            if (!sendResponse(connfd, response))
                running = false;
            return;
        }

        // Open the destination file in binary mode
        // Use a temporary file so failed transfers do not overwrite existing files
        string tempName = fileName + ".uploadXXXXXX";
        int tempfd = mkstemp(&tempName[0]);
        FILE* fp = (tempfd >= 0) ? fdopen(tempfd, "wb") : nullptr;

        if (fp == nullptr)
        {
            if (tempfd >= 0)
                close(tempfd);
            unlink(tempName.c_str());
            close(datafd);
            response = "550 CANNOT CREATE FILE\n";
            if (!sendResponse(connfd, response))
                running = false;
            return;
        }

        // Tell the client the server is ready
        ready = "150 READY\n";

        if (!sendResponse(connfd, ready))
        {
            fclose(fp);
            unlink(tempName.c_str());
            close(datafd);
            running = false;
            return;
        }

        // Receive and save the uploaded file
        while (true)
        {
            ssize_t bytesReceived = recv(
                datafd,
                totalFileBuffer,
                sizeof(totalFileBuffer),
                0
            );

            if (bytesReceived < 0)
            {
                if (errno == EINTR)
                    continue;
                perror("File receive");
                transferSuccess = false;
                break;
            }

            if (bytesReceived == 0)
            {
                break;
            }

            size_t bytesWritten = fwrite(
                totalFileBuffer,
                1,
                static_cast<size_t>(bytesReceived),
                fp
            );

            if (bytesWritten != static_cast<size_t>(bytesReceived))
            {
                perror("File write");
                transferSuccess = false;
                break;
            }

            totalReceived += bytesWritten;
        }

        // Close the file and data connection
        if (fclose(fp) != 0)
        {
            perror("File close");
            transferSuccess = false;
        }

        close(datafd);

        // Only replace the destination after receiving and saving the upload
        if (transferSuccess && rename(tempName.c_str(), fileName.c_str()) != 0)
        {
            perror("rename");
            transferSuccess = false;
        }
        if (!transferSuccess)
            unlink(tempName.c_str());

        // Report the transfer result
        if (transferSuccess)
        {
            cout << "PUT successful: " << fileName << " (" << totalReceived << " bytes)\n";

            response = "226 TRANSFER COMPLETE\n";

            if (!sendResponse(connfd, response))
                running = false;
        }
        else
        {
            cout << "PUT failed: " << fileName << '\n';

            response = "426 TRANSFER FAILED\n";

            if (!sendResponse(connfd, response))
                running = false;
        }

        return;
    }

    else if (firstLine == "ls")
    {
        // Nothing should follow the argument ls
        if (stream >> badInput)
        {
           response = "501 INVALID ARGUMENTS\n";
            if (!sendResponse(connfd, response))
                running = false;
            return;
        }

        // Cannot establish connection with temporary port
        if (dataPort == INVALID_DATA_PORT)
        {
            response = "425 DATA PORT NOT SET\n";
            if (!sendResponse(connfd, response))
                running = false;
            return;
        }

        // Contains directory listing
        string listing;

        // Call function, return failure if no listing exists
        if (!getDirectoryListing(listing))
        {
           string response = "550 DIRECTORY LISTING FAILED\n";
            if (!sendResponse(connfd, response))
                running = false;

            dataPort = INVALID_DATA_PORT;
            return;
        }

        // We'll use datafd to hold sockets for temp connection for transferring directory listing
        int datafd = connectToClientDataPort(clientAddr, dataPort);

        dataPort = INVALID_DATA_PORT;

        if (datafd < 0)
        {
            response = "425 DATA CONNECTION FAILED\n";
            if (!sendResponse(connfd, response))
            {
                running = false;
            }
            return;
        }

        ready = "150 READY\n";
        if (!sendResponse(connfd, ready))
        {
            close(datafd);
            running = false;
            return;
        }

        size_t totalSent = 0;
        transferSuccess = true;

        while (totalSent < listing.size())
        {
            ssize_t bytesSent = send(
                datafd,
                listing.data() + totalSent,
                listing.size() - totalSent,
                MSG_NOSIGNAL
            );

            if (bytesSent <= 0)
            {
                perror("Data transfer");
                transferSuccess = false;
                break;
            }

            totalSent += bytesSent;
        }

        // close temporary connection
        close(datafd);

        if (transferSuccess)
        {
            cout << "Directory listing sent successfully.\n";

            response = "226 TRANSFER COMPLETE\n";
            if (!sendResponse(connfd, response))
                running = false;
        }
        else
        {
            response = "426 TRANSFER FAILED\n";
            if (!sendResponse(connfd, response))
                running = false;
        }
    }

    else if (firstLine == "quit")
    {
        if (stream >> badInput)
        {
            response = "501 INVALID ARGUMENTS\n";
            if (!sendResponse(connfd, response))
                running = false;
            return;
        }

        cout << "Client has disconnected\n";
        response = "221 GOODBYE\n";
        if (!sendResponse(connfd, response))
            running = false;
        running = false;
    }
    // else return false
    else
    {
        cout << "Error: Invalid command, try \"get <filename>\" ,\"put <filename>\",\"ls\", or \"quit\"" << endl;
        response = "500 UNKNOWN COMMAND\n";
        if (!sendResponse(connfd, response))
            running = false;
        return;
    }
}

// Precondition: Requires connfd to have a valid connected socket and message to send back to the client
// Postcondition: If returns true, successfully sends message to client
bool sendResponse(int connfd, const string& message)
{
    // Keep a reference to the formatted reply that will be sent over the control connection.
    const string& response = message;
    // Track how many bytes have been sent so far.
    size_t total = 0;

    // A single send() may send only part of the message, so repeat until it is complete.
    while (total < response.size())
    {
        // Send the remaining bytes, starting where the previous send() stopped.
        ssize_t bytesSent = send(connfd, response.data() + total, response.size() - total, MSG_NOSIGNAL);
        // Zero or a negative result means the message could not be completed.
        if (bytesSent <= 0)
        {
            perror("send");
            return false;
        }

        // Advance past the bytes that were successfully sent.
        total += bytesSent;
    }

    // Log the response so the server prints command success/failure status.
    cout << "FTP response: " << response;
    return true;
}

// Precondition: Command needs a string to write into and return
// Postcondition: Stores and returns directory listing in string on success
bool getDirectoryListing(string& listing)
{
    // Store the result of the ls command when its process finishes
    int status;
    // Read the command output a small piece at a time
    char buffer[COMMAND_BUFFER_SIZE];
    // Run ls -1 and open a pipe to read the filenames it prints
    FILE* pipe = popen("ls -1", "r");

    // If the command could not be started, the listing cannot be retrieved
    if (pipe == nullptr)
    {
        perror("popen");
        return false;
    }

    // Clear any earlier listing before collecting the current one
    listing.clear();

    // Append each piece of ls output to the final listing string
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        listing += buffer;
    }

    // Close the pipe and check whether ls finished successfully
    status = pclose(pipe);

    if (status != 0)
    {
        return false;
    }
    else
    {
        return true;
    }
}

// Precondition: Requites a valid client's address and the client to listen on the specifed temporary port
// Postcondition: Returns connected data socket on success
int connectToClientDataPort(const sockaddr_in& clientAddr, int dataPort)
{
    // Create another socket for transferring files and directory listings
    int datafd = socket(AF_INET, SOCK_STREAM, 0);
    
    // A negative descriptor means Linux could not create the data socket
    if (datafd < 0)
    {
        perror("Data socket");
        return -1;
    }

    // Replace original port number with ephermeral port (We want client data port, not control port)
    // Keep the client IP address but replace its control port with the data port
    sockaddr_in dataAddr = clientAddr;
    dataAddr.sin_port = htons(dataPort);

    // Establish connection, else return failure
    if (connect(datafd, (struct sockaddr*)&dataAddr, sizeof(dataAddr)) < 0)
    {
        perror("Data connection");
        close(datafd);
        return -1;
    }

    // Bound stalled reads or writes on this temporary data connection
    // Use the configured timeout for data reads and writes
    timeval timeout{};
    timeout.tv_sec = DATA_TIMEOUT_SECONDS;
    // Apply receive and send timeouts; close the socket if either setting fails
    if (setsockopt(datafd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) < 0 ||
        setsockopt(datafd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) < 0)
    {
        perror("Data socket timeout");
        close(datafd);
        return -1;
    }

    cout << "Connected to client data port: " << dataPort << '\n';
    
    // Return socket descriptor
    return datafd;
}

