// FTP client for the FTP server.

#include "TCPLib.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <iostream>
#include <sstream>
#include <string>

using namespace std;

// GLOBAL CONSTANTS - this should mirror server's
const size_t FILE_BUFFER_SIZE = 4096;
const size_t MAX_RESPONSE_LENGTH = 4096;
const int DATA_LISTEN_BACKLOG = 1;
const int DATA_ACCEPT_TIMEOUT_MS = 30000;

// Reply codes used by the server
const int CODE_READY = 150;
const int CODE_OK = 200;
const int CODE_GOODBYE = 221;
const int CODE_TRANSFER_COMPLETE = 226;

// The control connection and any reply bytes received but not yet processed
struct ControlConnection {
  int fd;
  string pending;
};

// Returns a socket connected to the server, -1 on failure
int connectToServer(const char *host, const char *port);
// Sends command followed by '\n'; returns false if the connection failed
bool sendCommand(ControlConnection &ctrl, const string &command);
// Stores one reply line (without '\n') in line; returns false if the connection
// failed
bool readResponse(ControlConnection &ctrl, string &line);
// Returns the 3-digit reply code at the start of line, or -1
int responseCode(const string &line);
// Returns a socket listening on an ephemeral port and stores the port in
// dataPort, or -1
int createDataListener(int &dataPort);
// Returns a data listening socket the server accepted with PORT, or -1.
// controlOk is false if the connection failed.
int openDataListener(ControlConnection &ctrl, bool &controlOk);
// Returns the server's data connection, or -1 on timeout or error
int acceptDataConnection(int listenfd);
// Server - Client Command Functions
// Each command handler returns false only if the control connection is no
// longer usable - checks if connection was used on function call
bool doGet(ControlConnection &ctrl, const string &fileName);
bool doPut(ControlConnection &ctrl, const string &localName);
bool dols(ControlConnection &ctrl);
bool doQuit(ControlConnection &ctrl);
void printHelp();

int main(int argc, char **argv) {
  // check the command line: cli <server machine> <server port>
  if (argc != 3) {
    cerr << "USAGE: " << argv[0] << " <server machine> <server port>\n";
    return 1;
  }

  // writing to a socket the server closed should report EPIPE, not kill the
  // client
  signal(SIGPIPE, SIG_IGN);

  ControlConnection ctrl;
  ctrl.fd = connectToServer(argv[1], argv[2]);

  // early exit if connection error
  if (ctrl.fd < 0) {
    return 1;
  }

  cout << "Connected to " << argv[1] << " on port " << argv[2] << '\n';

  bool connected = true;
  string input;

  // main ftp loop
  while (connected) {
    cout << "ftp> " << flush;

    // End of input (Ctrl-D) ends the session cleanly
    if (!getline(cin, input)) {
      cout << '\n';
      doQuit(ctrl);
      break;
    }

    istringstream stream(input);
    string command;
    string argument;
    string extra;

    // Ignore blank lines
    if (!(stream >> command)) {
      continue;
    }

    stream >> argument;
    bool hasExtra = static_cast<bool>(stream >> extra);

    // check which command got/gets executed
    if (command == "get" || command == "put") {
      if (argument.empty() || hasExtra) {
        cout << "Usage: " << command << " <filename>\n";
        continue;
      }

      if (command == "get") {
        connected = doGet(ctrl, argument);
      } else {
        connected = doPut(ctrl, argument);
      }
    } else if (command == "ls" || command == "quit" || command == "help") {
      if (!argument.empty()) {
        cout << "Usage: " << command << " (no arguments)\n";
        continue;
      }

      if (command == "ls") {
        connected = dols(ctrl);
      } else if (command == "help") {
        printHelp();
      } else {
        doQuit(ctrl);
        connected = false;
      }
    } else {
      cout << "Unknown command: " << command << '\n';
      printHelp();
    }
  }

  // close connection and exit
  close(ctrl.fd);
  return 0;
}

// !!! from example client.cpp and getaddrinfo.cpp !!!
// Returns a socket connected to the server, or -1 on failure
int connectToServer(const char *host, const char *port) {
  char *endptr;
  long portNumber = strtol(port, &endptr, 10);

  if (*endptr != '\0' || portNumber < 1 || portNumber > 65535) {
    cerr << "Invalid port number. Use 1-65535.\n";
    return -1;
  }

  // Resolve the server machine name (from getaddrinfo.cpp). IPv4 only, because
  // the server and the data listener use IPv4.
  addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo *resolution = nullptr;
  int status = getaddrinfo(host, port, &hints, &resolution);

  if (status != 0) {
    cerr << "getaddrinfo: " << gai_strerror(status) << '\n';
    return -1;
  }

  int connfd = -1;

  // IPs can resolve to more than 1 domain; check every ip is said domain
  for (addrinfo *entry = resolution; entry != nullptr; entry = entry->ai_next) {
    connfd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);

    // check if connection is made
    if (connfd < 0) {
      continue;
    }

    if (connect(connfd, entry->ai_addr, entry->ai_addrlen) == 0) {
      break;
    }

    close(connfd);
    connfd = -1;
  }

  freeaddrinfo(resolution);

  // Less than 0 is an error
  if (connfd < 0) {
    cerr << "Could not connect to " << host << " on port " << port << ": "
         << strerror(errno) << '\n';
  }

  return connfd;
}

//!!! skeleton example client.cpp
// Sends command followed by '\n'; returns false if the connection failed
bool sendCommand(ControlConnection &ctrl, const string &command) {
  string message = command + "\n";

  if (tcp_send(ctrl.fd, message.data(), static_cast<int>(message.size())) < 0) {
    perror("send");
    return false;
  }

  return true;
}

// Stores one reply line (without '\n') in line; returns false if the connection
// failed
bool readResponse(ControlConnection &ctrl, string &line) {
  char buffer[512];
  size_t newlinePos;

  // TCP is a byte stream: keep reading until a complete line has arrived
  while ((newlinePos = ctrl.pending.find('\n')) == string::npos) {
    if (ctrl.pending.size() > MAX_RESPONSE_LENGTH) {
      cerr << "Server reply too long.\n";
      return false;
    }

    ssize_t bytesReceived = recv(ctrl.fd, buffer, sizeof(buffer), 0);

    if (bytesReceived < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("recv");
      return false;
    }

    if (bytesReceived == 0) {
      cerr << "Server closed the control connection.\n";
      return false;
    }

    ctrl.pending.append(buffer, static_cast<size_t>(bytesReceived));
  }

  line = ctrl.pending.substr(0, newlinePos);
  ctrl.pending.erase(0, newlinePos + 1);

  if (!line.empty() && line[line.size() - 1] == '\r') {
    line.erase(line.size() - 1);
  }

  cout << "Server: " << line << '\n';
  return true;
}

// Returns the 3-digit reply code at the start of line, or -1
int responseCode(const string &line) {
  if (line.size() < 3) {
    return -1;
  }

  for (size_t i = 0; i < 3; ++i) {
    if (line[i] < '0' || line[i] > '9') {
      return -1;
    }
  }

  return atoi(line.substr(0, 3).c_str());
}

// !!! from example getephport.cpp
// Returns a socket listening on an ephemeral port and stores the port in
// dataPort, or -1
int createDataListener(int &dataPort) {
  int listenfd = socket(AF_INET, SOCK_STREAM, 0);

  if (listenfd < 0) {
    perror("socket");
    return -1;
  }

  sockaddr_in dataAddr;
  memset(&dataAddr, 0, sizeof(dataAddr));
  dataAddr.sin_family = AF_INET;
  dataAddr.sin_addr.s_addr = htonl(INADDR_ANY);

  // Port 0 lets the OS pick a free ephemeral port (from getephport.cpp)
  dataAddr.sin_port = htons(0);

  socklen_t addrSize = sizeof(dataAddr);

  if (bind(listenfd, (sockaddr *)&dataAddr, addrSize) < 0) {
    perror("bind");
    close(listenfd);
    return -1;
  }

  if (listen(listenfd, DATA_LISTEN_BACKLOG) < 0) {
    perror("listen");
    close(listenfd);
    return -1;
  }

  // Find out which port bind picked
  if (getsockname(listenfd, (sockaddr *)&dataAddr, &addrSize) < 0) {
    perror("getsockname");
    close(listenfd);
    return -1;
  }

  dataPort = ntohs(dataAddr.sin_port);
  return listenfd;
}

// Returns a data listening socket the server accepted with PORT, or -1.
// controlOk is false if the connection failed.
int openDataListener(ControlConnection &ctrl, bool &controlOk) {
  controlOk = true;

  int dataPort;
  int listenfd = createDataListener(dataPort);

  if (listenfd < 0) {
    return -1;
  }

  // The client must be listening before the server tries to connect
  string line;

  if (!sendCommand(ctrl, "PORT " + to_string(dataPort)) ||
      !readResponse(ctrl, line)) {
    close(listenfd);
    controlOk = false;
    return -1;
  }

  if (responseCode(line) != CODE_OK) {
    close(listenfd);
    return -1;
  }

  return listenfd;
}

// Returns the server's data connection, or -1 on timeout or error
int acceptDataConnection(int listenfd) {
  pollfd waitFor;
  waitFor.fd = listenfd;
  waitFor.events = POLLIN;
  waitFor.revents = 0;

  int ready;

  // Wait a bounded time so a server that never connects cannot hang the client
  do {
    ready = poll(&waitFor, 1, DATA_ACCEPT_TIMEOUT_MS);
  } while (ready < 0 && errno == EINTR);

  if (ready < 0) {
    perror("poll");
    return -1;
  }

  if (ready == 0) {
    cerr << "Timed out waiting for the server's data connection.\n";
    return -1;
  }

  int datafd = accept(listenfd, nullptr, nullptr);

  if (datafd < 0) {
    perror("accept");
  }

  return datafd;
}

// Downloads fileName from the server into the current directory. Returns false
// only if  connection failed.
bool doGet(ControlConnection &ctrl, const string &fileName) {
  bool controlOk;
  int listenfd = openDataListener(ctrl, controlOk);

  if (listenfd < 0) {
    return controlOk;
  }

  string line;

  if (!sendCommand(ctrl, "get " + fileName) || !readResponse(ctrl, line)) {
    close(listenfd);
    return false;
  }

  // 150 is the success transfer code, only transfer if 150 is read
  if (responseCode(line) != CODE_READY) {
    close(listenfd);
    cout << "get failed.\n";
    return true;
  }

  int datafd = acceptDataConnection(listenfd);
  close(listenfd);

  if (datafd < 0) {
    // the server reports the failed transfer on the control connection
    return readResponse(ctrl, line);
  }

  // open the local file for writing; "wb" truncates any existing file
  FILE *fp = fopen(fileName.c_str(), "wb");

  if (fp == nullptr) {
    perror("Cannot create local file");
    close(datafd);
    return readResponse(ctrl, line);
  }

  char buffer[FILE_BUFFER_SIZE];
  size_t totalReceived = 0;
  bool localSuccess = true;

  // the server closes the data connection when the whole file is sent
  while (true) {
    ssize_t bytesReceived = recv(datafd, buffer, sizeof(buffer), 0);

    if (bytesReceived < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("Data receive");
      localSuccess = false;
      break;
    }

    if (bytesReceived == 0) {
      break;
    }

    size_t bytesWritten =
        fwrite(buffer, 1, static_cast<size_t>(bytesReceived), fp);

    if (bytesWritten != static_cast<size_t>(bytesReceived)) {
      perror("File write");
      localSuccess = false;
      break;
    }

    totalReceived += bytesWritten;
  }

  if (fclose(fp) != 0) {
    perror("File close");
    localSuccess = false;
  }

  close(datafd);

  // wait for the server's final status
  bool replyOk = readResponse(ctrl, line);
  bool serverSuccess = replyOk && responseCode(line) == CODE_TRANSFER_COMPLETE;

  if (localSuccess && serverSuccess) {
    cout << "Downloaded " << fileName << " (" << totalReceived << " bytes)\n";
  } else {
    // The local file is left with whatever was received before the failure
    cout << "get failed: " << fileName << " (" << totalReceived
         << " bytes received)\n";
  }

  return replyOk;
}

// !!! from example client.cpp
// Uploads localName to the server, saved there under its base name. Returns
// false only if the control connection failed.
bool doPut(ControlConnection &ctrl, const string &localName) {
  // Check the local file before involving the server
  int fd = open(localName.c_str(), O_RDONLY);

  if (fd < 0) {
    perror(localName.c_str());
    return true;
  }

  struct stat fileInfo;

  if (fstat(fd, &fileInfo) < 0 || !S_ISREG(fileInfo.st_mode)) {
    cout << localName << " is not a regular file.\n";
    close(fd);
    return true;
  }

  // The server only accepts simple names, so send the part after the last '/'
  string remoteName = localName.substr(localName.rfind('/') + 1);

  if (remoteName.empty()) {
    cout << "Invalid file name: " << localName << '\n';
    close(fd);
    return true;
  }

  bool controlOk;
  int listenfd = openDataListener(ctrl, controlOk);

  if (listenfd < 0) {
    close(fd);
    return controlOk;
  }

  string line;

  if (!sendCommand(ctrl, "put " + remoteName) || !readResponse(ctrl, line)) {
    close(fd);
    close(listenfd);
    return false;
  }

  if (responseCode(line) != CODE_READY) {
    close(fd);
    close(listenfd);
    cout << "put failed.\n";
    return true;
  }

  int datafd = acceptDataConnection(listenfd);
  close(listenfd);

  if (datafd < 0) {
    close(fd);
    return readResponse(ctrl, line);
  }

  // Send the whole file with sendfile() (from client.cpp)
  off_t offset = 0;
  bool localSuccess = true;

  while (offset < fileInfo.st_size) {
    ssize_t bytesSent = sendfile(
        datafd, fd, &offset, static_cast<size_t>(fileInfo.st_size - offset));

    if (bytesSent < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("sendfile");
      localSuccess = false;
      break;
    }

    // The file got shorter while it was being sent
    if (bytesSent == 0) {
      cerr << "Unexpected end of " << localName << '\n';
      localSuccess = false;
      break;
    }
  }

  close(fd);

  // Closing the data connection tells the server the file is complete
  close(datafd);

  bool replyOk = readResponse(ctrl, line);

  if (localSuccess && replyOk && responseCode(line) == CODE_TRANSFER_COMPLETE) {
    cout << "Uploaded " << localName << " as " << remoteName << " (" << offset
         << " bytes)\n";
  } else {
    cout << "put failed: " << localName << '\n';
  }

  return replyOk;
}

// !!! from popen.cpp
// Prints the server's directory listing. Returns false only if the control
// connection failed.
bool doLs(ControlConnection &ctrl) {
  bool controlOk;
  int listenfd = openDataListener(ctrl, controlOk);

  if (listenfd < 0) {
    return controlOk;
  }

  string line;

  if (!sendCommand(ctrl, "ls") || !readResponse(ctrl, line)) {
    close(listenfd);
    return false;
  }

  if (responseCode(line) != CODE_READY) {
    close(listenfd);
    cout << "ls failed.\n";
    return true;
  }

  int datafd = acceptDataConnection(listenfd);
  close(listenfd);

  if (datafd < 0) {
    return readResponse(ctrl, line);
  }

  char buffer[FILE_BUFFER_SIZE];
  string listing;

  while (true) {
    ssize_t bytesReceived = recv(datafd, buffer, sizeof(buffer), 0);

    if (bytesReceived < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("Data receive");
      break;
    }

    if (bytesReceived == 0) {
      break;
    }

    listing.append(buffer, static_cast<size_t>(bytesReceived));
  }

  close(datafd);

  cout << listing;
  cout << "(" << listing.size() << " bytes)\n";

  return readResponse(ctrl, line);
}

// Tells the server the session is over; quit
bool doQuit(ControlConnection &ctrl) {
  string line;

  if (!sendCommand(ctrl, "quit") || !readResponse(ctrl, line)) {
    return false;
  }

  return responseCode(line) == CODE_GOODBYE;
}

// Prints the available commands to tell the server
void printHelp() {
  cout << "Commands:\n"
       << "  get <file>  download a file from the server\n"
       << "  put <file>  upload a file to the server\n"
       << "  ls          list files on the server\n"
       << "  quit        disconnect and exit\n";
}
