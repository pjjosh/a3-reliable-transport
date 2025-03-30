#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <cxxopts.hpp>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "common/Crc32.hpp"
#include "common/PacketHeader.hpp"

using namespace std;

enum PacketType : unsigned int
{
    START = 0,
    END = 1,
    DATA = 2,
    ACK = 3
};
const unsigned int PACKET_SIZE = 1472;
const unsigned int HEADER_SIZE = sizeof(PacketHeader);
const unsigned int PAYLOAD_SIZE = PACKET_SIZE - HEADER_SIZE;

// struct for sending both header and payload in one udp datagram
struct WTPPacket
{
    PacketHeader header;
    char payload[PAYLOAD_SIZE];
};

// Convert a PacketHeader to network byte order
PacketHeader toNetworkOrder(const PacketHeader &pkt)
{
    PacketHeader netPkt;
    netPkt.type = htonl(pkt.type);
    netPkt.seqNum = htonl(pkt.seqNum);
    netPkt.length = htonl(pkt.length);
    netPkt.checksum = htonl(pkt.checksum);
    return netPkt;
}

// common send_packet fn: sends header + payload in one datagram
bool send_packet(int sockfd, const PacketHeader &packet,
                 const vector<char> &data, const sockaddr_in *addr,
                 socklen_t slen, ofstream &logger)
{
    char send_buffer[PACKET_SIZE];
    memset(send_buffer, 0, sizeof(send_buffer));

    // convert header to network order
    PacketHeader netHeader = toNetworkOrder(packet);
    memcpy(send_buffer, &netHeader, HEADER_SIZE);

    // copy payload if any, copying only packet.length bytes (up to PAYLOAD_SIZE)
    if (!data.empty() && packet.length > 0)
    {
        size_t copySize = min((size_t)packet.length, (size_t)PAYLOAD_SIZE);
        memcpy(send_buffer + HEADER_SIZE, data.data(), copySize);
    }

    size_t totalSize = HEADER_SIZE + packet.length;
    size_t sent_bytes = 0;
    // loop to send over all the bytes
    while (sent_bytes < totalSize)
    {
        int curr_sent =
            sendto(sockfd, send_buffer + sent_bytes, totalSize - sent_bytes, 0,
                   (struct sockaddr *)addr, slen);
        if (curr_sent < 0)
        {
            cerr << "failed to send packet" << endl;
            return false;
        }
        sent_bytes += curr_sent;
    }
    // output logger for sent packet
    string type_str =
        (packet.type == START
             ? "START"
             : (packet.type == END ? "END"
                                   : (packet.type == DATA ? "DATA" : "ACK")));
    logger << type_str << " " << packet.seqNum << " " << packet.length << " "
           << packet.checksum << "\n";
    logger.flush();
    return true;
}

pair<WTPPacket, bool> receive_packet(int sockfd, sockaddr_in *client_addr,
                                     socklen_t &slen, ofstream &logger)
{
    char recv_buffer[PACKET_SIZE];
    memset(recv_buffer, 0, sizeof(recv_buffer));
    int recv_len = recvfrom(sockfd, recv_buffer, PACKET_SIZE, 0,
                            (struct sockaddr *)client_addr, &slen);
    if (recv_len < (int)HEADER_SIZE)
    {
        // cerr << "error: recvfrom failed or too few bytes" << endl;
        // nothing to receive, error, or too few bytes
        return {WTPPacket{}, true}; // indicate error/corruption
    }

    WTPPacket pkt;
    // Copy out the header from the buffer
    memcpy(&pkt.header, recv_buffer, HEADER_SIZE);
    // Convert header fields from network order to host order
    pkt.header.type = ntohl(pkt.header.type);
    pkt.header.seqNum = ntohl(pkt.header.seqNum);
    pkt.header.length = ntohl(pkt.header.length);
    pkt.header.checksum = ntohl(pkt.header.checksum);

    // If there's a payload, ensure we received all of it and copy it into
    // pkt.payload
    if (pkt.header.length > 0)
    {
        if (recv_len < (int)(HEADER_SIZE + pkt.header.length))
        {
            cerr << "error: incomplete datagram received" << endl;
            return {WTPPacket{}, true};
        }
        memcpy(pkt.payload, recv_buffer + HEADER_SIZE, pkt.header.length);
        // (The remaining bytes in pkt.payload remain zeroed)
    }

    // Verify checksum (if there is a payload)
    bool corrupted = false;
    if (pkt.header.length > 0)
    {
        uint32_t computed = crc32(pkt.payload, pkt.header.length);
        if (computed != pkt.header.checksum)
        {
            corrupted = true;
        }
    }

    // Log the received header info
    string type_str = (pkt.header.type == START
                           ? "START"
                           : (pkt.header.type == END
                                  ? "END"
                                  : (pkt.header.type == DATA ? "DATA" : "ACK")));
    logger << type_str << " " << pkt.header.seqNum << " " << pkt.header.length
           << " " << pkt.header.checksum << "\n";
    logger.flush();

    return {pkt, corrupted};
}

// Function to create and return a UDP socket.
int createUDPSocket()
{
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        cerr << "Error: Could not create socket" << endl;
        exit(1);
    }
    return sockfd;
}

// Main wReceiver implementation.
int main(int argc, char *argv[])
{
    // Parse command-line arguments.
    cxxopts::Options options("wReceiver", "Reliable WTP Receiver");
    options.add_options()("p,port", "Port number on which wReceiver listens",
                          cxxopts::value<int>())(
        "w,window-size", "Maximum number of outstanding packets",
        cxxopts::value<int>()->default_value("10"))(
        "d,output-dir", "Directory to store output files (FILE-i.out)",
        cxxopts::value<string>())(
        "o,output-log", "File path for logging receiver activity",
        cxxopts::value<string>()->default_value("receiver.out"));

    auto result = options.parse(argc, argv);
    int port = result["port"].as<int>();
    int windowSize = result["window-size"].as<int>();
    string outputDir = result["output-dir"].as<string>();
    string outputLog = result["output-log"].as<string>();

    // Open log file.
    ofstream logStream(outputLog, ios::app);
    if (!logStream.is_open())
    {
        cerr << "Error: Could not open log file " << outputLog << endl;
        exit(1);
    }

    // Create and bind a UDP socket.
    int sockfd = createUDPSocket();
    sockaddr_in myAddr;
    memset((char *)&myAddr, 0, sizeof(myAddr)); // Clearingi the structure.
    myAddr.sin_family = AF_INET;
    myAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    myAddr.sin_port = htons(port);
    if (::bind(sockfd, (struct sockaddr *)&myAddr, sizeof(myAddr)) < 0)
    {
        perror("bind failed");
        exit(1);
    }
    // cout << "wReceiver listening on port " << port << endl;

    // Connection state variables.
    bool connectionActive = false;
    unsigned int expectedSeq = 0;
    unsigned int connectionStartSeq = 0; // from START packet
    vector<char> fileBuffer;
    map<unsigned int, vector<char>> outOfOrderPackets;
    int connectionCount = 0;
    unsigned int fileCounter = 0;

    sockaddr_in clientAddr;
    socklen_t clen = sizeof(clientAddr);

    // Main loop: wReceiver never terminates.
    while (true)
    {
        auto recvResult = receive_packet(sockfd, &clientAddr, clen, logStream);
        if (recvResult.second)
        {
            cerr << "Error: Received corrupted packet" << endl;
            continue;
        }
        PacketHeader header = recvResult.first.header;

        // Process packet based on type.
        if (header.type == START)
        {
            if (connectionActive)
            {
                // Already in a connection; ignore extra START messages.
                // spec said to only do 1 connection at a time
                continue;
            }
            else
            {
                connectionActive = true;
                expectedSeq = 0;
                connectionStartSeq = header.seqNum;
                fileBuffer.clear();
                outOfOrderPackets.clear();
                cout << "Connection started. Start seq: " << connectionStartSeq << endl;
                // Create a new output file.
                stringstream filestream;
                filestream << outputDir << "/FILE-" << connectionCount << ".out";
                string filename = filestream.str();
                ofstream outputFile(filename, ios::binary);
                if (!outputFile.is_open())
                {
                    cerr << "Error: Cannot open output file " << filename << endl;
                    exit(1);
                }
                outputFile.close();
                // Send ACK for expected data (0) using send_packet.
                PacketHeader ackPkt;
                ackPkt.type = ACK;
                ackPkt.seqNum = connectionStartSeq;
                ackPkt.length = 0;
                ackPkt.checksum = 0;
                send_packet(sockfd, ackPkt, vector<char>(), &clientAddr, clen,
                            logStream);
            }
        }
        else if (!connectionActive)
        {
            // Ignore any packets other than START when no connection is active.
            // This works because it comes before any if case relating to DATA or END
            continue;
        }
        else if (header.type == DATA)
        {
            // we get here when we have an active connection and receive a DATA
            // packet. Here, payload is part of recvResult.first.
            vector<char> payload(recvResult.first.payload,
                                 recvResult.first.payload + header.length);
            uint32_t computed = crc32(payload.data(), payload.size());
            if (computed != header.checksum)
            {
                cout << "Dropped DATA packet seq " << header.seqNum
                     << " due to checksum mismatch." << endl;
                continue;
            }
            if (header.seqNum >= expectedSeq + windowSize)
                continue; // drop packet if it is outside the window
            if (header.seqNum == expectedSeq)
            {
                // if the packet is in order
                // Insert the payload into the fileBuffer
                fileBuffer.insert(fileBuffer.end(), payload.begin(), payload.end());
                expectedSeq++;
                while (outOfOrderPackets.find(expectedSeq) != outOfOrderPackets.end())
                {
                    vector<char> &buf = outOfOrderPackets[expectedSeq];
                    fileBuffer.insert(fileBuffer.end(), buf.begin(), buf.end());
                    outOfOrderPackets.erase(expectedSeq);
                    expectedSeq++;
                }
            }
            else if (header.seqNum > expectedSeq)
            {
                // if the packet is out of order
                // Store the payload in the outOfOrderPackets map
                if (outOfOrderPackets.find(header.seqNum) == outOfOrderPackets.end())
                    outOfOrderPackets[header.seqNum] = payload;
            }
            PacketHeader ackPkt;
            ackPkt.type = ACK;
            ackPkt.seqNum = header.seqNum; // << THIS IS THE ONLY CHANGE THAT WAS MADE FOR OPT
            ackPkt.length = 0;
            ackPkt.checksum = 0;
            send_packet(sockfd, ackPkt, vector<char>(), &clientAddr, clen,
                        logStream);
        }
        else if (header.type == END)
        {
            if (header.seqNum == connectionStartSeq)
            {
                PacketHeader ackPkt;
                ackPkt.type = ACK;
                ackPkt.seqNum = connectionStartSeq;
                ackPkt.length = 0;
                ackPkt.checksum = 0;
                send_packet(sockfd, ackPkt, vector<char>(), &clientAddr, clen,
                            logStream);
                string outFilename =
                    outputDir + "/FILE-" + to_string(connectionCount) + ".out";
                ofstream outfile(outFilename, ios::binary);
                if (outfile.is_open())
                {
                    outfile.write(fileBuffer.data(), fileBuffer.size());
                    outfile.close();
                    cout << "Wrote received file to " << outFilename << endl;
                }
                else
                {
                    cerr << "Error: Could not open output file " << outFilename << endl;
                }
                connectionActive = false;
                connectionCount++;
            }
            // if the END packet is received with a seqNum that doesn't match the
            // expected start sequence number, we can ignore
        }
    }

    close(sockfd);
    return 0;
}