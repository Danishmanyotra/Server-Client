#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <cstring>
#include <queue>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <cstdint>
#include <cstdio>

#pragma comment(lib, "ws2_32.lib") // link winsock library

#define PORT_RECEIVE 5050          // Port for receiving files from listener
#define PORT_SEND 5051             // Port for sending files to listener
#define CHUNK_SIZE 65536           // 64KB chunks for large files
#define MAX_CONCURRENT_THREADS 100 // Limit concurrent threads to avoid resource exhaustion

// Helper: send all bytes from buf
bool sendAllBytes(SOCKET sock, const char *buf, int len)
{
    int total = 0;
    while (total < len)
    {
        int sent = send(sock, buf + total, len - total, 0);
        if (sent == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            std::cerr << "Send error: " << err << " (WSAECONNRESET=" << WSAECONNRESET
                      << ", WSAECONNABORTED=" << WSAECONNABORTED << ")\n";
            return false;
        }
        if (sent == 0)
        {
            std::cerr << "Send returned 0 (connection closed by remote)\n";
            return false;
        }
        total += sent;
    }
    return true;
}

// Helper: receive exactly len bytes into buf
bool recvExactBytes(SOCKET sock, char *buf, int len)
{
    int total = 0;
    while (total < len)
    {
        int recvd = recv(sock, buf + total, len - total, 0);
        if (recvd <= 0)
        {
            std::cerr << "Recv error or connection closed: " << WSAGetLastError() << "\n";
            return false;
        }
        total += recvd;
    }
    return true;
}

// Send file using length-prefixed protocol
// Format: [4-byte filename_len][filename][8-byte file_size][file_data]
bool sendFile(SOCKET sock, const std::string &filename, const std::string &filepath)
{
    std::ifstream infile(filepath, std::ios::binary);
    if (!infile)
    {
        std::cerr << "Cannot open file: " << filepath << "\n";
        return false;
    }

    // Get file size
    infile.seekg(0, std::ios::end);
    long long fileSize = infile.tellg();
    infile.seekg(0, std::ios::beg);

    std::cout << "Sending file: " << filename << " (" << fileSize << " bytes)\n";

    // Send filename length (4 bytes, little-endian)
    int fnLen = static_cast<int>(filename.size());
    char lenBuf[4];
    lenBuf[0] = (fnLen >> 0) & 0xFF;
    lenBuf[1] = (fnLen >> 8) & 0xFF;
    lenBuf[2] = (fnLen >> 16) & 0xFF;
    lenBuf[3] = (fnLen >> 24) & 0xFF;
    if (!sendAllBytes(sock, lenBuf, 4))
        return false;

    // Send filename
    if (!sendAllBytes(sock, filename.c_str(), fnLen))
        return false;

    // Send file size (8 bytes, little-endian)
    char sizeBuf[8];
    for (int i = 0; i < 8; i++)
        sizeBuf[i] = (fileSize >> (i * 8)) & 0xFF;
    if (!sendAllBytes(sock, sizeBuf, 8))
        return false;

    // Compute CRC32 for the file (so receiver can verify integrity)
    // CRC32 polynomial 0xEDB88320
    static uint32_t crc_table[256];
    static bool crc_table_init = false;
    if (!crc_table_init)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
            crc_table[i] = c;
        }
        crc_table_init = true;
    }

    auto crc32_update = [&](uint32_t crc, const char *buf, size_t len) -> uint32_t
    {
        uint32_t c = crc ^ 0xFFFFFFFFu;
        for (size_t k = 0; k < len; ++k)
            c = crc_table[(c ^ (unsigned char)buf[k]) & 0xFFu] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    };

    // Read through file to compute CRC
    uint32_t runningCrc = 0xFFFFFFFFu;
    infile.clear();
    infile.seekg(0, std::ios::beg);
    char crcChunk[CHUNK_SIZE];
    long long scanned = 0;
    while (scanned < fileSize)
    {
        int toRead = (fileSize - scanned > CHUNK_SIZE) ? CHUNK_SIZE : static_cast<int>(fileSize - scanned);
        infile.read(crcChunk, toRead);
        std::streamsize r = infile.gcount();
        if (r <= 0)
            break;
        runningCrc = crc32_update(runningCrc, crcChunk, static_cast<size_t>(r));
        scanned += r;
    }
    uint32_t fileCrc = runningCrc;

    // Send CRC32 (4 bytes, little-endian)
    char crcBuf[4];
    crcBuf[0] = (fileCrc >> 0) & 0xFF;
    crcBuf[1] = (fileCrc >> 8) & 0xFF;
    crcBuf[2] = (fileCrc >> 16) & 0xFF;
    crcBuf[3] = (fileCrc >> 24) & 0xFF;
    if (!sendAllBytes(sock, crcBuf, 4))
        return false;

    // Rewind file back to start and send file data in chunks
    infile.clear();
    infile.seekg(0, std::ios::beg);

    // Send file data in chunks
    char chunk[CHUNK_SIZE];
    long long sent = 0;
    while (sent < fileSize)
    {
        int toRead = (fileSize - sent > CHUNK_SIZE) ? CHUNK_SIZE : static_cast<int>(fileSize - sent);
        infile.read(chunk, toRead);
        if (!sendAllBytes(sock, chunk, toRead))
            return false;
        sent += toRead;
    }

    infile.close();
    std::cout << "File sent successfully.\n";
    return true;
}

// Receive file using length-prefixed protocol
// Format: [4-byte filename_len][filename][8-byte file_size][file_data]
bool receiveFile(SOCKET sock)
{
    // Receive filename length
    char lenBuf[4];
    if (!recvExactBytes(sock, lenBuf, 4))
        return false;
    int fnLen = (lenBuf[0] & 0xFF) | ((lenBuf[1] & 0xFF) << 8) |
                ((lenBuf[2] & 0xFF) << 16) | ((lenBuf[3] & 0xFF) << 24);

    // Receive filename
    std::string filename(fnLen, '\0');
    if (!recvExactBytes(sock, &filename[0], fnLen))
        return false;

    // Receive file size
    char sizeBuf[8];
    if (!recvExactBytes(sock, sizeBuf, 8))
        return false;
    long long fileSize = 0;
    for (int i = 0; i < 8; i++)
        fileSize |= ((long long)(sizeBuf[i] & 0xFF)) << (i * 8);

    std::cout << "Receiving file: " << filename << " (" << fileSize << " bytes)\n";

    // Generate output filename with _copy suffix
    std::string outFilename;
    size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos)
        outFilename = filename + "_copy";
    else
        outFilename = filename.substr(0, dot) + "_copy" + filename.substr(dot);

    // Receive file data and write to disk
    std::ofstream outfile(outFilename, std::ios::binary);
    if (!outfile)
    {
        std::cerr << "Cannot create output file: " << outFilename << "\n";
        return false;
    }

    char chunk[CHUNK_SIZE];
    long long recvd = 0;
    while (recvd < fileSize)
    {
        int toRecv = (fileSize - recvd > CHUNK_SIZE) ? CHUNK_SIZE : static_cast<int>(fileSize - recvd);
        if (!recvExactBytes(sock, chunk, toRecv))
        {
            outfile.close();
            return false;
        }
        outfile.write(chunk, toRecv);
        recvd += toRecv;
    }

    outfile.close();
    std::cout << "File received and saved: " << outFilename << "\n";
    return true;
}

// Global thread pool management
std::queue<std::function<void()>> taskQueue;
std::mutex queueMutex;
std::condition_variable queueCV;
std::atomic<int> activeThreads(0);
std::atomic<bool> shouldShutdown(false);

// Thread pool worker
void threadPoolWorker()
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCV.wait(lock, []
                         { return !taskQueue.empty() || shouldShutdown; });

            if (shouldShutdown && taskQueue.empty())
                break;

            if (!taskQueue.empty())
            {
                task = taskQueue.front();
                taskQueue.pop();
            }
        }

        if (task)
        {
            task();
            activeThreads--;
        }
    }
}

// Submit a task to the thread pool
void submitTask(std::function<void()> task)
{
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        taskQueue.push(task);
        activeThreads++;
    }
    queueCV.notify_one();
}

int main(int argc, char *argv[])
{
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cout << "WSAStartup failed!\n";
        return 1;
    }

    // Read file to send (default: data.txt)
    const std::string filename = "data.txt";
    std::string filepath = filename;

    std::ifstream file(filepath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Error: Could not read " << filepath << "\n";
        WSACleanup();
        return 1;
    }
    file.close();

    // Allow optional port argument: sender.exe [base_port]
    // base_port + 0 = receive port, base_port + 1 = send port
    int basePort = PORT_RECEIVE;
    if (argc >= 2)
    {
        int p = atoi(argv[1]);
        if (p > 0)
            basePort = p;
    }

    int receivePort = basePort;
    int sendPort = basePort + 1;

    std::cout << "Sender: Starting dual-port server...\n";
    std::cout << "  Receive port (listeners send files here): " << receivePort << "\n";
    std::cout << "  Send port (listeners receive files from here): " << sendPort << "\n";

    // Print local IP addresses for convenience
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0)
    {
        struct hostent *he = gethostbyname(hostname);
        if (he)
        {
            std::cout << "Local IPs:\n";
            for (int i = 0; he->h_addr_list[i] != NULL; ++i)
            {
                struct in_addr addr;
                memcpy(&addr, he->h_addr_list[i], sizeof(struct in_addr));
                std::cout << " - " << inet_ntoa(addr) << std::endl;
            }
        }
    }

    // Lambda: create and bind a server socket
    auto createServerSocket = [](int port) -> SOCKET
    {
        SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == INVALID_SOCKET)
        {
            std::cerr << "Socket creation failed for port " << port << "!\n";
            return INVALID_SOCKET;
        }

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(fd, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
        {
            std::cerr << "Bind failed for port " << port << "!\n";
            closesocket(fd);
            return INVALID_SOCKET;
        }

        if (listen(fd, SOMAXCONN) == SOCKET_ERROR)
        {
            std::cerr << "Listen failed for port " << port << "!\n";
            closesocket(fd);
            return INVALID_SOCKET;
        }

        return fd;
    };

    // Create both server sockets
    SOCKET recvSocket = createServerSocket(receivePort);
    SOCKET sendSocket = createServerSocket(sendPort);

    if (recvSocket == INVALID_SOCKET || sendSocket == INVALID_SOCKET)
    {
        WSACleanup();
        return 1;
    }

    std::cout << "Ready to accept connections. Press Ctrl+C to stop.\n";

    // Start thread pool workers
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) // 4 worker threads to handle queued tasks
        workers.push_back(std::thread(threadPoolWorker));

    // Lambda: accept connections on a port (send-only or receive-only)
    auto acceptOnPort = [&](SOCKET serverSock, int port, bool isSendMode)
    {
        while (true)
        {
            sockaddr_in clientAddr;
            int clientLen = sizeof(clientAddr);
            SOCKET clientSock = accept(serverSock, (sockaddr *)&clientAddr, &clientLen);
            if (clientSock == INVALID_SOCKET)
            {
                std::cerr << "Accept failed on port " << port << " (continuing)...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            std::cout << "Client connected on port " << port << ": "
                      << inet_ntoa(clientAddr.sin_addr) << ":"
                      << ntohs(clientAddr.sin_port)
                      << " (Active threads: " << activeThreads << ", Queue size: " << taskQueue.size() << ")\n";

            // Submit task to thread pool instead of spawning detached thread
            submitTask([clientSock, filename, filepath, isSendMode, port]()
                       {
                try
                {
                    if (isSendMode)
                    {
                        // Send-only: send file to client
                        std::cout << "Sending file on port " << port << "...\n";
                        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Give client time to be ready
                        if (!sendFile(clientSock, filename, filepath))
                            std::cerr << "Failed to send file on port " << port << "\n";
                        else
                            std::cout << "File sent successfully on port " << port << "\n";
                    }
                    else
                    {
                        // Receive-only: receive file from client
                        std::cout << "Receiving file on port " << port << "...\n";
                        if (!receiveFile(clientSock))
                            std::cerr << "Failed to receive file on port " << port << "\n";
                        else
                            std::cout << "File received successfully on port " << port << "\n";
                    }
                    closesocket(clientSock);
                }
                catch (const std::exception &e)
                {
                    std::cerr << "Exception on port " << port << ": " << e.what() << "\n";
                    closesocket(clientSock);
                } });
        }
    };

    // Start two threads: one for receive port, one for send port
    // Port 5050: Listener RECEIVES from Sender (sender in send mode)
    // Port 5051: Listener SENDS to Sender (sender in receive mode)
    std::thread recvThread(acceptOnPort, recvSocket, receivePort, true); // Send to listener on port 5050
    std::thread sendThread(acceptOnPort, sendSocket, sendPort, false);   // Receive from listener on port 5051

    // Keep main thread alive (these will block forever)
    recvThread.join();
    sendThread.join();

    // Shutdown thread pool
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        shouldShutdown = true;
    }
    queueCV.notify_all();

    for (auto &w : workers)
        if (w.joinable())
            w.join();

    closesocket(recvSocket);
    closesocket(sendSocket);
    WSACleanup();
    return 0;
}
