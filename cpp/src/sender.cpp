#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cxxopts.hpp>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "common/Crc32.hpp"
#include "common/PacketHeader.hpp"

using namespace std;

// define packet sizes (ethernet MTU minus headers: 1500 - 20 (ip) - 8 (udp))
const unsigned int PACKET_SIZE = 1472;
const unsigned int HEADER_SIZE = sizeof(PacketHeader);
const unsigned int PAYLOAD_SIZE = PACKET_SIZE - HEADER_SIZE;

// enum for packet types
enum PacketType : unsigned int { START = 0, END = 1, DATA = 2, ACK = 3 };

// struct for sending both header and payload in one udp datagram
struct WTPPacket {
  PacketHeader header;
  char payload[PAYLOAD_SIZE];  // we use PAYLOAD_SIZE only
};

// Convert a PacketHeader to network byte order
PacketHeader toNetworkOrder(const PacketHeader &pkt) {
  PacketHeader netPkt;
  netPkt.type = htonl(pkt.type);
  netPkt.seqNum = htonl(pkt.seqNum);
  netPkt.length = htonl(pkt.length);
  netPkt.checksum = htonl(pkt.checksum);
  return netPkt;
}

// create a udp socket
int createUDPSocket() {
  int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sockfd < 0) {
    cerr << "error: couldn't create socket" << endl;
    exit(1);
  }
  return sockfd;
}

// setup receiver addr from hostname & port
sockaddr_in setupReceiverAddress(const string &hostname, int port) {
  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, hostname.c_str(), &addr.sin_addr) <= 0) {
    cerr << "invalid address: " << hostname << endl;
    exit(1);
  }

  return addr;
}

// build a start pkt with a random seqnum
PacketHeader buildStartPacket() {
  PacketHeader pkt;
  pkt.type = START;
  static random_device rd;
  static mt19937 gen(rd());
  uniform_int_distribution<unsigned int> dis(
      0, numeric_limits<unsigned int>::max());
  pkt.seqNum = dis(gen);
  pkt.length = 0;
  pkt.checksum = 0;
  return pkt;
}

// build a data pkt; calc checksum over data only
PacketHeader buildDataPacket(const vector<char> &data, unsigned int seqNum) {
  PacketHeader pkt;
  pkt.type = DATA;
  pkt.seqNum = seqNum;
  pkt.length = data.size();
  pkt.checksum = crc32(data.data(), data.size());
  return pkt;
}

// build an end pkt; use same seqnum as start pkt
PacketHeader buildEndPacket(unsigned int startSeqNum) {
  PacketHeader pkt;
  pkt.type = END;
  pkt.seqNum = startSeqNum;
  pkt.length = 0;
  pkt.checksum = 0;
  return pkt;
}

// common send_packet fn: sends header + payload in one datagram
bool send_packet(int sockfd, const PacketHeader &packet,
                 const vector<char> &data, const sockaddr_in *addr,
                 socklen_t slen, ofstream &logger) {
  char send_buffer[PACKET_SIZE];
  memset(send_buffer, 0, sizeof(send_buffer));

  // convert header to network order
  PacketHeader netHeader = toNetworkOrder(packet);
  memcpy(send_buffer, &netHeader, HEADER_SIZE);

  // copy payload if any, copying only packet.length bytes (up to PAYLOAD_SIZE)
  if (!data.empty() && packet.length > 0) {
    size_t copySize = min((size_t)packet.length, (size_t)PAYLOAD_SIZE);
    memcpy(send_buffer + HEADER_SIZE, data.data(), copySize);
  }

  size_t totalSize = HEADER_SIZE + packet.length;
  size_t sent_bytes = 0;
  // loop to send over all the bytes
  while (sent_bytes < totalSize) {
    int curr_sent =
        sendto(sockfd, send_buffer + sent_bytes, totalSize - sent_bytes, 0,
               (struct sockaddr *)addr, slen);
    if (curr_sent < 0) {
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
                                     socklen_t &slen, ofstream &logger) {
  char recv_buffer[PACKET_SIZE];
  memset(recv_buffer, 0, sizeof(recv_buffer));
  int recv_len = recvfrom(sockfd, recv_buffer, PACKET_SIZE, 0,
                          (struct sockaddr *)client_addr, &slen);
  if (recv_len < (int)HEADER_SIZE) {
    cerr << "error: recvfrom failed or too few bytes" << endl;
    return {WTPPacket{}, true};  // indicate error/corruption
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
  if (pkt.header.length > 0) {
    if (recv_len < (int)(HEADER_SIZE + pkt.header.length)) {
      cerr << "error: incomplete datagram received" << endl;
      return {WTPPacket{}, true};
    }
    memcpy(pkt.payload, recv_buffer + HEADER_SIZE, pkt.header.length);
    // (The remaining bytes in pkt.payload remain zeroed)
  }

  // Verify checksum (if there is a payload)
  bool corrupted = false;
  if (pkt.header.length > 0) {
    uint32_t computed = crc32(pkt.payload, pkt.header.length);
    if (computed != pkt.header.checksum) {
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

// read input file, split it into chunks, and build data pkts
vector<PacketHeader> packetizeFile(const string &inputFile,
                                   vector<vector<char>> &dataChunks) {
  vector<PacketHeader> packets;
  ifstream file(inputFile, ios::binary);
  if (!file) {
    cerr << "couldn't open input file: " << inputFile << endl;
    exit(1);
  }
  unsigned int seqNum = 0;
  while (true) {
    vector<char> buffer(PAYLOAD_SIZE);
    file.read(buffer.data(), PAYLOAD_SIZE);
    streamsize bytesRead = file.gcount();
    if (bytesRead <= 0) break;
    buffer.resize(bytesRead);
    dataChunks.push_back(buffer);
    PacketHeader pkt = buildDataPacket(buffer, seqNum);
    packets.push_back(pkt);
    seqNum++;
  }
  return packets;
}

int main(int argc, char *argv[]) {
  cxxopts::Options options("wSender", "reliable wtp sender");
  options.add_options()("h,hostname",
                        "ip address of the host that wreceiver is runnin on",
                        cxxopts::value<string>())(
      "p,port", "port number on which wreceiver is listenin",
      cxxopts::value<int>())("w,window-size",
                             "max number of outstanding pkts in current window",
                             cxxopts::value<int>()->default_value("10"))(
      "i,input-file", "path to the file that has to be transferred",
      cxxopts::value<string>())(
      "o,output-log", "path to the file where wsender logs activity",
      cxxopts::value<string>()->default_value("sender.out"));

  auto result = options.parse(argc, argv);
  string hostname = result["hostname"].as<string>();
  int port = result["port"].as<int>();
  int windowSize = result["window-size"].as<int>();
  string inputFile = result["input-file"].as<string>();
  string outputLog = result["output-log"].as<string>();

  // open log file
  ofstream logger(outputLog, ios::app);
  if (!logger.is_open()) {
    cerr << "error: couldn't open log file " << outputLog << endl;
    exit(1);
  }

  int sockfd = createUDPSocket();
  sockaddr_in receiverAddr = setupReceiverAddress(hostname, port);
  socklen_t addrLen = sizeof(receiverAddr);

  timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 500000;
  // setting the socket timeout to 500 ms
  if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    cerr << "Error setting socket timeout" << endl;
    exit(1);
  }

  // send packet;receive ack for start
  PacketHeader startPacket = buildStartPacket();
  auto startsequencenum = startPacket.seqNum;  // save for later use
  char payload[PAYLOAD_SIZE] = {0};            // empty payload for start packet
  if (!send_packet(sockfd, startPacket,
                   vector<char>(payload, payload + PAYLOAD_SIZE), &receiverAddr,
                   addrLen, logger)) {
    cerr << "error: failed to send start packet" << endl;
    exit(1);
  }

  pair<WTPPacket, bool> recv_data =
      receive_packet(sockfd, &receiverAddr, addrLen, logger);
  if (recv_data.second || recv_data.first.header.type != PacketType::ACK ||
      recv_data.first.header.seqNum != startsequencenum) {
    cerr << "error: failed to receive ACK for start packet or received "
            "corrupted packet"
         << endl;
    exit(1);
  }

  // packetize file & use sliding window
  vector<vector<char>> dataChunks;
  vector<PacketHeader> dataPackets = packetizeFile(inputFile, dataChunks);
  size_t totalPackets = dataPackets.size();
  size_t base = 0;           // index of first unacked packet
  size_t nextToSend = base;  // index of next packet to send

  // send initial window
  while (nextToSend < totalPackets && nextToSend < base + windowSize) {
    send_packet(sockfd, dataPackets[nextToSend], dataChunks[nextToSend],
                &receiverAddr, addrLen, logger);
    nextToSend++;
  }

  // --- sliding window loop ---
  while (base < totalPackets) {
    // try to receive an ACK packet using our recv_packet function
    auto ackResult = receive_packet(sockfd, &receiverAddr, addrLen, logger);
    // if an error occurred or the packet is corrupted, treat it as a timeout:
    if (ackResult.second) {
      // retransmit all pkts in the current window (base to nextToSend-1)
      for (size_t i = base; i < nextToSend; i++) {
        send_packet(sockfd, dataPackets[i], dataChunks[i], &receiverAddr,
                    addrLen, logger);
      }
      continue;
    }
    // extract the ACK packet header
    PacketHeader ackPkt = ackResult.first.header;
    // check if this ack advances our window
    if (ackPkt.type == ACK) {
      // if the ACK is for a packet we have sent
      if (ackPkt.seqNum > base) {
        size_t shift = ackPkt.seqNum - base;
        base += shift;
        // send new packets in the shifted window
        while (nextToSend < totalPackets && nextToSend < base + windowSize) {
          send_packet(sockfd, dataPackets[nextToSend], dataChunks[nextToSend], &receiverAddr, addrLen, logger);
          nextToSend++;
        }
      }
    }
  }

  // send end packet until ack recvd
  PacketHeader endPacket = buildEndPacket(startPacket.seqNum);
  // loop to send end packet until we receive an ACK for it
  while (true) {
    if (!send_packet(sockfd, endPacket, vector<char>(), &receiverAddr, addrLen,
                     logger))
      continue;
    auto ackResult = receive_packet(sockfd, &receiverAddr, addrLen, logger);
    // if an error occurred or packet is corrupted, try again
    if (ackResult.second) continue;

    // match
    if (ackResult.first.header.type == ACK &&
        ackResult.first.header.seqNum == endPacket.seqNum)
      break;
  }

  close(sockfd);
  logger.close();
  return 0;
}
