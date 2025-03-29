#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>

#include <cxxopts.hpp>

#include "common/Crc32.hpp"          // Provides: uint32_t crc32(const void*, size_t)
#include "common/PacketHeader.hpp"   // Provides definition for PacketHeader

using namespace std;

enum PacketType : unsigned int { START = 0, END = 1, DATA = 2, ACK = 3 };


// Function to create and return a UDP socket.
int createUDPSocket() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        cerr << "Error: Could not create socket" << endl;
        exit(1);
    }
    return sockfd;
}

// Log a packet event in the format: <type> <seqNum> <length> <checksum>
void logPacket(ofstream &logStream, const string &type, unsigned int seqNum,
               unsigned int length, unsigned int checksum) {
    logStream << type << " " << seqNum << " " << length << " " << checksum << "\n";
    logStream.flush();
}

// Helper: Send an ACK packet with ackSeq as the cumulative ACK.
void sendAck(int sockfd, const sockaddr_in &senderAddr, unsigned int ackSeq,
             ofstream &logStream) {
    PacketHeader ack;
    ack.type = ACK;
    ack.seqNum = ackSeq;
    ack.length = 0;
    ack.checksum = 0;
    sendto(sockfd, &ack, sizeof(ack), 0, (struct sockaddr *)&senderAddr, sizeof(senderAddr));
    logPacket(logStream, "ACK", ackSeq, 0, 0);
}

// Main wReceiver implementation.
int main(int argc, char* argv[]) {
    // Parse command-line arguments.
    cxxopts::Options options("wReceiver", "Reliable WTP Receiver");
    options.add_options()
        ("p,port", "Port number on which wReceiver listens",
         cxxopts::value<int>())
        ("w,window-size", "Maximum number of outstanding packets",
         cxxopts::value<int>()->default_value("10"))
        ("d,output-dir", "Directory to store output files (FILE-i.out)",
         cxxopts::value<string>())
        ("o,output-log", "File path for logging receiver activity",
         cxxopts::value<string>()->default_value("receiver.out"));
    
    auto result = options.parse(argc, argv);
    int port = result["port"].as<int>();
    int windowSize = result["window-size"].as<int>();
    string outputDir = result["output-dir"].as<string>();
    string outputLog = result["output-log"].as<string>();

    // Open log file.
    ofstream logStream(outputLog, ios::app);
    if (!logStream.is_open()) {
        cerr << "Error: Could not open log file " << outputLog << endl;
        return 1;
    }

    // Create and bind a UDP socket.
    int sockfd = createUDPSocket();
    sockaddr_in myAddr;
    memset(&myAddr, 0, sizeof(myAddr));
    myAddr.sin_family = AF_INET;
    myAddr.sin_addr.s_addr = INADDR_ANY;
    myAddr.sin_port = htons(port);
    if (::bind(sockfd, (struct sockaddr *)&myAddr, sizeof(myAddr)) < 0) {
        perror("bind failed");
        return 1;
    }
    cout << "wReceiver listening on port " << port << endl;

    // Connection state variables.
    bool connectionActive = false;
    unsigned int expectedSeq = 0;
    unsigned int connectionStartSeq = 0; // From the START packet.
    vector<char> fileBuffer;              // Accumulated file data.
    // Buffer for out-of-order DATA packets: maps seqNum -> payload.
    map<unsigned int, vector<char>> outOfOrderPackets;
    int connectionCount = 0;  // For naming output files as FILE-0.out, etc.

    // Main loop: wReceiver never terminates.
    while (true) {
        // Receive a packet header.
        PacketHeader header;
        sockaddr_in senderAddr;
        socklen_t addrLen = sizeof(senderAddr);
        ssize_t headerLen = recvfrom(sockfd, &header, sizeof(header), 0,
                                     (struct sockaddr *)&senderAddr, &addrLen);
        if (headerLen < (ssize_t)sizeof(header))
            continue;  // Malformed packet.

        // Log the received header.
        logPacket(logStream, "RECV", header.seqNum, header.length, header.checksum);

        // Receive payload if any.
        vector<char> payload;
        if (header.length > 0) {
            payload.resize(header.length);
            ssize_t dataLen = recvfrom(sockfd, payload.data(), header.length, 0,
                                       (struct sockaddr *)&senderAddr, &addrLen);
            if (dataLen != (ssize_t)header.length)
                continue;  // Error: payload not fully received.
        }

        // Process based on packet type.
        if (header.type == START) {
            if (connectionActive) {
                // Already in a connection; ignore further START messages.
                continue;
            } else {
                // Begin a new connection.
                connectionActive = true;
                expectedSeq = 0;             // Expect DATA packets starting at 0.
                connectionStartSeq = header.seqNum; // Remember sender's random start seq.
                fileBuffer.clear();
                outOfOrderPackets.clear();
                cout << "Connection started. Start seq: " << connectionStartSeq << endl;
                // Immediately send an ACK for the expected DATA (0).
                sendAck(sockfd, senderAddr, expectedSeq, logStream);
            }
        } else if (!connectionActive) {
            // Ignore any packets other than START when no connection is active.
            continue;
        } else if (header.type == DATA) {
            // Verify checksum: compute over payload and compare.
            uint32_t computed = crc32(payload.data(), payload.size());
            if (computed != header.checksum) {
                // Malformed packet: drop it without sending ACK.
                cout << "Dropped DATA packet seq " << header.seqNum << " due to checksum mismatch." << endl;
                continue;
            }
            // Check if packet is within the allowed window.
            if (header.seqNum >= expectedSeq + windowSize) {
                // Outside window; drop it.
                continue;
            }
            if (header.seqNum == expectedSeq) {
                // In-order packet: append payload to file buffer.
                fileBuffer.insert(fileBuffer.end(), payload.begin(), payload.end());
                expectedSeq++;
                // Check buffered out-of-order packets for contiguous sequence.
                while (outOfOrderPackets.find(expectedSeq) != outOfOrderPackets.end()) {
                    vector<char> &buf = outOfOrderPackets[expectedSeq];
                    fileBuffer.insert(fileBuffer.end(), buf.begin(), buf.end());
                    outOfOrderPackets.erase(expectedSeq);
                    expectedSeq++;
                }
                sendAck(sockfd, senderAddr, expectedSeq, logStream);
            } else if (header.seqNum > expectedSeq) {
                // Out-of-order packet within window: buffer it if not already received.
                if (outOfOrderPackets.find(header.seqNum) == outOfOrderPackets.end())
                    outOfOrderPackets[header.seqNum] = payload;
                // Always send cumulative ACK.
                sendAck(sockfd, senderAddr, expectedSeq, logStream);
            }
        } else if (header.type == END) {
            // Check that END packet comes from the same connection (using the same start seq).
            if (header.seqNum == connectionStartSeq) {
                // Send ACK for END message.
                sendAck(sockfd, senderAddr, expectedSeq, logStream);
                // Write the accumulated file data to disk.
                string outFilename = outputDir + "/FILE-" + to_string(connectionCount) + ".out";
                ofstream outfile(outFilename, ios::binary);
                if (outfile.is_open()) {
                    outfile.write(fileBuffer.data(), fileBuffer.size());
                    outfile.close();
                    cout << "Wrote received file to " << outFilename << endl;
                } else {
                    cerr << "Error: Could not open output file " << outFilename << endl;
                }
                // End connection and prepare for next.
                connectionActive = false;
                connectionCount++;
            } else {
                // END packet from an unexpected connection; ignore.
                continue;
            }
        }
    }
    close(sockfd);
    return 0;
}
