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

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace std;


static void ensure_dir(const std::string& dir) {
    struct stat st{};
    if (stat(dir.c_str(), &st) == -1) {
        mkdir(dir.c_str(), 0777);
    }
}

static void print_usage(const char* prog) {
    cout
        << "Usage:\n"
        << "  " << prog << " [-c] -p <1..15> -t <0..59.996> -e <1|2>\n"
        << "  " << prog << " [-c] -p <1..15>\n"
        << "  " << prog << " [-c] -f <filename>        (relative to BIMDC/, saves to received/<filename>)\n"
        << "  Optional: -m <buffercap bytes>\n";
}

// format value to match BIMDC
static std::string fmt_ecg(double v) {
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss << std::setprecision(3) << v;     // 3 decimals then trim
    std::string s = oss.str();
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s.empty()) s = "0";               // safety
    return s;
}

// ---------- protocol helpers ----------

static double request_ecg(FIFORequestChannel& ch, int person, double seconds, int ecgno) {
    datamsg dm(person, seconds, ecgno);
    ch.cwrite(&dm, sizeof(dm));
    double val = 0.0;
    ch.cread(&val, sizeof(val));
    return val;
}

static void write_first_1000_csv(FIFORequestChannel& ch, int person, const std::string& outpath) {
    ensure_dir("received");
    std::ofstream out(outpath, std::ios::binary | std::ios::trunc);
    if (!out) { std::cerr << "Failed to open " << outpath << " for write\n"; std::exit(1); }

    for (int i = 0; i < 1000; ++i) {
        double tt = i * 0.004;                 // 0, 0.004, ..., 3.996
        double v1 = request_ecg(ch, person, tt, 1);
        double v2 = request_ecg(ch, person, tt, 2);

        // BIMDC style for ALL three columns: up to 3 decimals, trimmed zeros
        out << fmt_ecg(tt) << ","
            << fmt_ecg(v1) << ","
            << fmt_ecg(v2) << "\n";
    }
    out.close();
}

static void download_file(FIFORequestChannel& ch, const std::string& fname, int buffercap) {
    // request size (offset=0,length=0) + filename in ONE write
    const size_t namebytes = fname.size() + 1;
    std::vector<char> req(sizeof(filemsg) + namebytes);
    filemsg header0(0, 0);
    std::memcpy(req.data(), &header0, sizeof(filemsg));
    std::memcpy(req.data() + sizeof(filemsg), fname.c_str(), namebytes);

    ch.cwrite(req.data(), req.size());
    __int64_t fsize = 0;
    ch.cread(&fsize, sizeof(fsize));
    if (fsize < 0) {
        std::cerr << "Server reports file not found: " << fname << "\n";
        return;
    }

    ensure_dir("received");
    const std::string outpath = "received/" + fname;
    std::ofstream out(outpath, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "Failed to open " << outpath << " for write\n";
        std::exit(1);
    }

    auto t0 = std::chrono::steady_clock::now();

    __int64_t offset = 0;
    while (offset < fsize) {
        int chunk = static_cast<int>(std::min<__int64_t>(buffercap, fsize - offset));
        filemsg fm(offset, chunk);
        std::memcpy(req.data(), &fm, sizeof(filemsg));
        // filename bytes already sit after the header in req; we only replace the header each loop

        ch.cwrite(req.data(), req.size());

        std::vector<char> data(chunk);
        ch.cread(data.data(), chunk);
        out.write(data.data(), chunk);

        offset += chunk;
    }

    out.close();
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    std::cout << "Received \"" << fname << "\" -> " << outpath
              << " (" << fsize << " bytes) in " << std::fixed << std::setprecision(3)
              << secs << " s with buffercap=" << buffercap << "\n";
}

static FIFORequestChannel* get_new_channel(FIFORequestChannel& control) {
    MESSAGE_TYPE m = NEWCHANNEL_MSG;
    control.cwrite(&m, sizeof(m));
    char namebuf[256] = {0};
    control.cread(namebuf, sizeof(namebuf));
    return new FIFORequestChannel(namebuf, FIFORequestChannel::CLIENT_SIDE);
}

int main(int argc, char* argv[]) {
    // ---- parse CLI ----
    bool flag_c = false;
    bool have_p = false, have_t = false, have_e = false, have_f = false;
    int p = 0, e = 0;
    double t = 0.0;
    std::string filename;
    int buffercap = MAX_MESSAGE;      // default 256 unless -m overrides

    int opt;
    // note: 'c' has no argument; p,t,e,f,m have arguments
    while ((opt = getopt(argc, argv, "cp:t:e:f:m:")) != -1) {
        switch (opt) {
            case 'c': flag_c = true; break;
            case 'p': p = std::atoi(optarg); have_p = true; break;
            case 't': t = std::atof(optarg); have_t = true; break;
            case 'e': e = std::atoi(optarg); have_e = true; break;
            case 'f': filename = optarg; have_f = true; break;
            case 'm': buffercap = std::max(1, std::atoi(optarg)); break;
            default:
                print_usage(argv[0]);
                return 0;
        }
    }

    // ---- 4.1: spawn server as child (pass -m) ----
    pid_t server_pid = fork();
    if (server_pid == 0) {
        // child -> exec server
        std::string mstr = std::to_string(buffercap);
        char* const args[] = {
            (char*)"./server",
            (char*)"-m", (char*)mstr.c_str(),
            nullptr
        };
        execvp(args[0], args);
        perror("execvp ./server");
        _exit(127);
    } else if (server_pid < 0) {
        perror("fork");
        return 1;
    }

    // connect control channel
    FIFORequestChannel control("control", FIFORequestChannel::CLIENT_SIDE);

    // optionally switch to a new data channel
    FIFORequestChannel* active = &control;
    FIFORequestChannel* aux = nullptr;
    if (flag_c) {
        aux = get_new_channel(control);
        active = aux;
    }

    // ---- decide action ----
    bool did_something = false;

    if (have_f) {
        // 4.3: file transfer
        download_file(*active, filename, buffercap);
        did_something = true;
    } else if (have_p && have_t && have_e) {
        // 4.2 (A): single datapoint
        double val = request_ecg(*active, p, t, e);

        std::cout << "For person " << p
                  << ", at time " << fmt_ecg(t)
                  << ", the value of ecg " << e
                  << " is " << fmt_ecg(val) << "\n";
        did_something = true;
    } else if (have_p && !have_t && !have_e) {
        // 4.2 (B): first 1000 rows for that patient -> received/x1.csv
        write_first_1000_csv(*active, p, "received/x1.csv");
        did_something = true;
    } else {
        // show usage if nothing specific requested (still clean shutdown)
        print_usage(argv[0]);
    }

    // ---- 4.5: close channels cleanly ----
    // if we opened a new channel, quit it first
    if (aux) {
        MESSAGE_TYPE q = QUIT_MSG;
        aux->cwrite(&q, sizeof(q));
        std::cout << "Client-side is done and exited\n";
        delete aux;
        aux = nullptr;
        active = &control;
    }

    // quit control channel
    {
        MESSAGE_TYPE q = QUIT_MSG;
        control.cwrite(&q, sizeof(q));
        std::cout << "Client-side is done and exited\n";
    }

    // wait for server child to end
    int status = 0;
    waitpid(server_pid, &status, 0);
    std::cout << "Server terminated\n";

    (void)did_something; // not used further, but left for clarity
    return 0;
}
