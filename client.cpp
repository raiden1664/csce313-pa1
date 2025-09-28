/*
    Original author of the starter code
    Tanzir Ahmed
    Department of Computer Science & Engineering
    Texas A&M University
    Date: 2/8/20

    Please include your Name, UIN, and the date below
    Name: Raiden Shipley
    UIN: 934003717
    Date: 09/28/2025
*/

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"
#include "FIFORequestChannel.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <algorithm>

using namespace std;

// ---------- 4.2 helpers ----------
static double request_ecg(FIFORequestChannel& ch, int person, double seconds, int ecgno) {
    datamsg dm(person, seconds, ecgno);
    ch.cwrite(&dm, sizeof(dm));
    double val = 0.0;
    ch.cread(&val, sizeof(val));
    return val;
}

static void write_first_1000_csv(FIFORequestChannel& ch, int person, const std::string& outpath) {
    ofstream out(outpath);
    if (!out) {
        cerr << "Failed to open " << outpath << " for write\n";
        exit(1);
    }
    for (int i = 0; i < 1000; ++i) {
        double tt = i * 0.004;
        double v1 = request_ecg(ch, person, tt, 1);
        double v2 = request_ecg(ch, person, tt, 2);
        out << std::fixed << setprecision(3) << tt << ","
            << setprecision(6) << v1 << ","
            << setprecision(6) << v2 << "\n";
    }
    out.close();
}

static bool nearly_equal(double a, double b, double eps) {
    return std::fabs(a - b) <= eps;
}

static bool parse_csv_row(const string& s, double& t, double& e1, double& e2) {
    std::stringstream ss(s);
    string f1, f2, f3;
    if (!std::getline(ss, f1, ',')) return false;
    if (!std::getline(ss, f2, ',')) return false;
    if (!std::getline(ss, f3, ',')) return false;
    try { t = std::stod(f1); e1 = std::stod(f2); e2 = std::stod(f3); }
    catch (...) { return false; }
    return true;
}

static bool compare_first_1000_against_original(int person, const std::string& gen_path, std::string& msg_out) {
    const std::string orig_path = "BIMDC/" + std::to_string(person) + ".csv";
    ifstream ga(gen_path), ob(orig_path);
    if (!ga) { msg_out = "Could not open generated file: " + gen_path; return false; }
    if (!ob) { msg_out = "Could not open original file: " + orig_path; return false; }

    // try to skip a header if the original has one
    string lb;
    double t0, e10, e20;
    streampos pos0 = ob.tellg();
    if (std::getline(ob, lb)) {
        if (!parse_csv_row(lb, t0, e10, e20)) {
            // header detected, keep going
        } else {
            ob.clear();
            ob.seekg(pos0); // rewind if first line was data
        }
    } else { msg_out = "Original file is empty."; return false; }

    string la;
    for (int i = 0; i < 1000; ++i) {
        if (!std::getline(ga, la)) { msg_out = "Generated file ended early at row " + to_string(i); return false; }
        if (!std::getline(ob, lb)) { msg_out = "Original file ended early at row " + to_string(i); return false; }
        double ta, a1, a2, tb, b1, b2;
        if (!parse_csv_row(la, ta, a1, a2)) { msg_out = "Parse error in generated line " + to_string(i); return false; }
        if (!parse_csv_row(lb, tb, b1, b2)) { msg_out = "Parse error in original line " + to_string(i); return false; }
        if (!nearly_equal(ta, tb, 1e-3) || !nearly_equal(a1, b1, 1e-6) || !nearly_equal(a2, b2, 1e-6)) {
            std::ostringstream os;
            os << "Mismatch at row " << i
               << ": got (" << std::fixed << setprecision(3) << ta
               << ", " << setprecision(6) << a1
               << ", " << setprecision(6) << a2
               << "), expected ("
               << setprecision(3) << tb
               << ", " << setprecision(6) << b1
               << ", " << setprecision(6) << b2 << ")";
            msg_out = os.str();
            return false;
        }
    }
    msg_out = "x1.csv matches BIMDC/" + std::to_string(person) + ".csv for the first 1000 rows.";
    return true;
}

// ---------- 4.3 helpers ----------
static std::string basename_only(const std::string& path) {
    size_t pos = path.find_last_of('/');
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}
static void ensure_dir(const char* dir) {
    struct stat st{};
    if (stat(dir, &st) == -1) mkdir(dir, 0777);
}
static void send_file_request(FIFORequestChannel& ch, const filemsg& fm, const std::string& fname) {
    const size_t req_len = sizeof(filemsg) + fname.size() + 1;
    std::vector<char> req(req_len);
    std::memcpy(req.data(), &fm, sizeof(filemsg));
    std::memcpy(req.data() + sizeof(filemsg), fname.c_str(), fname.size() + 1);
    ch.cwrite(req.data(), (int)req.size());
}
static int64_t get_file_size(FIFORequestChannel& ch, const std::string& fname) {
    filemsg fm0(0, 0);   // special: ask for size
    send_file_request(ch, fm0, fname);
    int64_t fsize = 0;
    ch.cread(&fsize, sizeof(fsize));
    return fsize;
}
static void transfer_file(FIFORequestChannel& ch, const std::string& fname, size_t buffercap) {
    const auto t0 = std::chrono::steady_clock::now();

    const int64_t total = get_file_size(ch, fname);
    if (total < 0) { cerr << "Server returned negative file size?!\n"; return; }

    ensure_dir("received");
    const std::string outname = "received/" + basename_only(fname);
    std::ofstream out(outname, std::ios::binary | std::ios::trunc);
    if (!out) { cerr << "Failed to open " << outname << " for write\n"; exit(1); }

    std::vector<char> chunk(buffercap);
    int64_t offset = 0;
    while (offset < total) {
        int to_read = (int)std::min<int64_t>((int64_t)buffercap, total - offset);
        filemsg fm(offset, to_read);
        send_file_request(ch, fm, fname);
        ch.cread(chunk.data(), to_read);
        out.write(chunk.data(), to_read);
        offset += to_read;
    }
    out.close();

    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "Received \"" << fname << "\" -> " << outname
              << " (" << total << " bytes) in " << std::fixed << setprecision(3)
              << sec << " s with buffercap=" << buffercap << "\n";
}

// --------------- Main ---------------
int main (int argc, char *argv[]) {
    // Detect which mode user wants
    int    p = -1;
    double t = -1.0;
    int    e = -1;
    std::string filename = "";
    size_t buffercap = MAX_MESSAGE;
    bool   use_new_channel = false;

    int opt;
    // NOTE: include c,f,m here
    while ((opt = getopt(argc, argv, "cp:t:e:f:m:")) != -1) {
        switch (opt) {
            case 'c': use_new_channel = true; break;
            case 'p': p = atoi(optarg); break;
            case 't': t = atof(optarg); break;
            case 'e': e = atoi(optarg); break;
            case 'f': filename = optarg; break;
            case 'm': buffercap = static_cast<size_t>(strtoull(optarg, nullptr, 10)); break;
            default: break;
        }
    }

    // 4.1: launch server as a child (forward -m to server if provided)
    pid_t server_pid = fork();
    if (server_pid == 0) {
        if (buffercap != MAX_MESSAGE) {
            std::string mstr = std::to_string(buffercap);
            execl("./server", "server", "-m", mstr.c_str(), (char*)NULL);
        } else {
            execl("./server", "server", (char*)NULL);
        }
        perror("exec ./server");
        _exit(127);
    } else if (server_pid < 0) {
        perror("fork");
        return 1;
    }

    // control channel
    FIFORequestChannel control("control", FIFORequestChannel::CLIENT_SIDE);

    // 4.4: (optional) request a new channel and use it for the actual work
    FIFORequestChannel* work = &control;               // default: use control
    std::unique_ptr<FIFORequestChannel> owned;         // holds the new channel if created

    if (use_new_channel) {
        MESSAGE_TYPE m = NEWCHANNEL_MSG;
        control.cwrite(&m, sizeof(m));                 // ask server to create a new channel
        char newname[1024] = {0};
        control.cread(newname, sizeof(newname));       // server replies with the name
        owned = std::make_unique<FIFORequestChannel>(newname, FIFORequestChannel::CLIENT_SIDE);
        work = owned.get();
        // (optional) std::cout << "Using new channel: " << newname << "\n";
    }

    // Modes: 4.3 > 4.2A > 4.2B
    if (!filename.empty()) {
        // 4.3: file transfer (filename relative to BIMDC/)
        transfer_file(*work, filename, buffercap);

    } else if (p >= 1 && p <= 15 && t >= 0.0 && (e == 1 || e == 2)) {
        // 4.2A: single datapoint
        double val = request_ecg(*work, p, t, e);
        cout.setf(std::ios::fixed);
        cout << setprecision(6)
             << "For person " << p
             << ", at time " << t
             << ", the value of ecg " << e
             << " is " << val << endl;

    } else if (p >= 1 && p <= 15 && t < 0.0 && e < 0) {
        // 4.2B: first 1000 points -> x1.csv + compare
        const std::string out = "x1.csv";
        write_first_1000_csv(*work, p, out);
        std::string msg;
        (void)compare_first_1000_against_original(p, out, msg);
        cout << msg << "\n";

    } else {
        cerr << "Usage:\n"
             << "  " << argv[0] << " [-c] -p <1..15> -t <0..59.996> -e <1|2>\n"
             << "  " << argv[0] << " [-c] -p <1..15>\n"
             << "  " << argv[0] << " [-c] -f <filename>        (relative to BIMDC/, saves to received/<filename>)\n"
             << "  Optional: -m <buffercap bytes>\n";
    }

    // Clean shutdown:
    // If we created a new channel, send QUIT on it first.
    if (owned) {
        MESSAGE_TYPE q = QUIT_MSG;
        work->cwrite(&q, sizeof(q));
        owned.reset(); // closes the FIFOs for the new channel on client side
    }
    // Then QUIT the control channel.
    MESSAGE_TYPE q2 = QUIT_MSG;
    control.cwrite(&q2, sizeof(q2));

    int status = 0;
    waitpid(server_pid, &status, 0);
    return 0;
}
