#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

#define PORT 5050
#define PORT_RECEIVE 5050 // Port to receive files from sender
#define PORT_SEND 5051    // Port to send files to sender
#define CHUNK_SIZE 65536  // 64KB chunks for large files

// Helper: send all bytes from buf
bool sendAllBytes(SOCKET sock, const char *buf, int len)
{
    int total = 0;
    while (total < len)
    {
        int sent = send(sock, buf + total, len - total, 0);
        if (sent == SOCKET_ERROR)
        {
            std::cerr << "Send error: " << WSAGetLastError() << "\n";
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

int main(int argc, char *argv[])
{
    WSADATA wsa;
    SOCKET sock = INVALID_SOCKET;
    struct sockaddr_in server;

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::cout << "Failed to initialize Winsock\n";
        return 1;
    }

    // Parse command-line args: listener.exe <sender_ip> [mode] [file_to_send]
    // Modes: "send" (send file to sender on port 5051), "receive" (receive file from sender on port 5050)
    // Default: "both" (receive then send)
    const char *server_ip = "127.0.0.1";
    int port = PORT;
    std::string mode = "both";
    std::string fileToSend = "";

    if (argc >= 2)
        server_ip = argv[1];
    if (argc >= 3)
    {
        // Check if argv[2] is a port number or a mode
        if (atoi(argv[2]) > 0)
        {
            port = atoi(argv[2]);
            if (argc >= 4)
                mode = argv[3];
            if (argc >= 5)
                fileToSend = argv[4];
        }
        else
        {
            mode = argv[2];
            if (argc >= 4)
                fileToSend = argv[3];
        }
    }

    // Determine actual port based on mode
    int actualPort = port;
    if (mode == "send")
        actualPort = PORT_SEND;
    else if (mode == "receive")
        actualPort = PORT_RECEIVE;
    // "both" mode: for backward compatibility, use PORT_RECEIVE first

    std::cout << "Listener mode: " << mode << "\n";

    // Create socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)
    {
        std::cout << "Could not create socket\n";
        WSACleanup();
        return 1;
    }

    // Setup server address
    server.sin_family = AF_INET;
    server.sin_port = htons(actualPort);
    server.sin_addr.s_addr = inet_addr(server_ip);

    // Connect to sender
    std::cout << "Connecting to sender at " << server_ip << ":" << actualPort << "...\n";
    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0)
    {
        std::cout << "Connection Failed! Sender not reachable at " << server_ip << ":" << actualPort << "\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    std::cout << "Connected to sender!\n";

    // Execute based on mode
    if (mode == "receive")
    {
        // Receive-only mode
        if (!receiveFile(sock))
        {
            std::cerr << "Failed to receive file from sender\n";
        }
    }
    else if (mode == "send")
    {
        // Send-only mode
        if (fileToSend.empty())
        {
            std::cout << "Error: No file specified to send. Usage: listener.exe <ip> send <filename>\n";
        }
        else if (!sendFile(sock, fileToSend, fileToSend))
        {
            std::cerr << "Failed to send file to sender\n";
        }
    }
    else if (mode == "both")
    {
        // Bidirectional mode (old behavior): receive first, then send
        if (!receiveFile(sock))
        {
            std::cerr << "Failed to receive file from sender\n";
            closesocket(sock);
            WSACleanup();
            return 1;
        }

        // Send file to sender if specified
        if (!fileToSend.empty())
        {
            std::cout << "Sending file: " << fileToSend << "\n";
            if (!sendFile(sock, fileToSend, fileToSend))
            {
                std::cerr << "Failed to send file to sender\n";
            }
        }
        else
        {
            std::cout << "No file to send. Closing connection.\n";
        }
    }
    else
    {
        std::cout << "Unknown mode: " << mode << ". Use 'send', 'receive', or 'both'.\n";
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
