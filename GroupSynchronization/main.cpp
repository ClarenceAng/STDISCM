#include <algorithm>
#include <chrono>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <new>
#include <random>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

#include "config.h"

using namespace std;

counting_semaphore<> freeInstances(0);
mutex mtx;

vector<string> status;
vector<int> partiesServed;
vector<long long> timeServed;
vector<thread> runs;

// a dungeon thread can't throw an exception to main, so it saves the first one here instead
exception_ptr runError;

chrono::steady_clock::time_point startTime;

double elapsed() {
    return chrono::duration<double>(chrono::steady_clock::now() - startTime).count();
}

void printStatus() {
    for (int i = 0; i < maxInstances; i++) {
        cout << "  Instance " << i + 1 << ": " << status[i] << "\n";
    }
}

// only call this while holding mtx
void printEvent(string message) {
    cout << "[" << fixed << setprecision(1) << elapsed() << "s] " << message << "\n";
    printStatus();
}

// the message inside a saved exception
string errorMessage(exception_ptr error) {
    try {
        rethrow_exception(error);
    } catch (exception& e) {
        return e.what();
    } catch (...) {
        return "unknown error";
    }
}

// empty instance that has served the least parties
int pickInstance() {
    int best = -1;
    for (int i = 0; i < maxInstances; i++) {
        if (status[i] == "empty" && (best == -1 || partiesServed[i] < partiesServed[best])) {
            best = i;
        }
    }
    return best;
}

// lock_guard unlocks mtx even if something throws, mtx.lock()/unlock() would leave it locked forever
void runDungeon(int instance, int party, int clearTime) {
    try {
        this_thread::sleep_for(chrono::seconds(clearTime));

        lock_guard<mutex> lock(mtx);
        status[instance] = "empty";
        partiesServed[instance]++;
        timeServed[instance] += clearTime;
        printEvent("Party " + to_string(party) + " cleared Instance " + to_string(instance + 1));
    } catch (...) {
        // an exception leaving this thread would end the whole program, so save it for main to report
        lock_guard<mutex> lock(mtx);
        status[instance] = "empty";
        if (!runError) runError = current_exception();
        try {
            printEvent("Party " + to_string(party) + " failed in Instance " + to_string(instance + 1));
        } catch (...) {
            // the error is already saved for main, only this one line of output is lost
        }
    }

    // give the instance back even after an error, otherwise enterDungeon could wait for it forever
    freeInstances.release();
}

void enterDungeon(int party, int clearTime) {
    // wait for a free instance before locking, otherwise it can deadlock
    freeInstances.acquire();

    int instance = -1;
    try {
        {
            lock_guard<mutex> lock(mtx);
            instance = pickInstance();
            status[instance] = "active";
            printEvent("Party " + to_string(party) + " entered Instance " + to_string(instance + 1) +
                       " (clear time: " + to_string(clearTime) + "s)");
        }

        if (runs[instance].joinable()) runs[instance].join();
        runs[instance] = thread(runDungeon, instance, party, clearTime);
    } catch (...) {
        // the party never started (e.g. the system couldn't make another thread), so free the instance again
        if (instance != -1) {
            lock_guard<mutex> lock(mtx);
            status[instance] = "empty";
            printEvent("Party " + to_string(party) + " could not start in Instance " + to_string(instance + 1));
        }
        freeInstances.release();
        throw;
    }
}

unsigned randomSeed() {
    try {
        return random_device{}();
    } catch (exception& e) {
        cerr << "Warning: no random device (" << e.what() << "), seeding the clear times from the clock instead\n";
        return (unsigned)chrono::steady_clock::now().time_since_epoch().count();
    }
}

int main(int argc, char* argv[]) {
    if (argc > 2) {
        cerr << "Usage: lfg [config file]   (the config file defaults to config.txt)\n";
        return 1;
    }
    string configFile = "config.txt";
    if (argc == 2) configFile = argv[1];

    try {
        loadConfig(configFile);
    } catch (ConfigError& e) {
        cerr << "Config error: " << e.what() << "\n";
        return 1;
    }

    long long parties = min({numTanks, numHealers, numDps / 3});
    long long leftTanks = numTanks - parties;
    long long leftHealers = numHealers - parties;
    long long leftDps = numDps - parties * 3;

    // n can be up to INT_MAX, which is far more instances than fit in memory
    try {
        status.assign(maxInstances, "empty");
        partiesServed.assign(maxInstances, 0);
        timeServed.assign(maxInstances, 0);
        runs.resize(maxInstances);
    } catch (bad_alloc&) {
        cerr << "Error: not enough memory for n = " << maxInstances << " instances, use a smaller n\n";
        return 1;
    }
    freeInstances.release(maxInstances);

    cout << "=== LFG Dungeon Queue ===\n";
    cout << "Max concurrent instances (n): " << maxInstances << "\n";
    cout << "Queue: " << numTanks << " tank(s), " << numHealers << " healer(s), " << numDps << " DPS\n";
    cout << "Clear time: " << minClearTime << "s to " << maxClearTime << "s\n\n";
    cout << "Parties formed: " << parties << "\n";
    cout << "Leftover players: " << leftTanks << " tank(s), " << leftHealers << " healer(s), " << leftDps << " DPS\n\n";
    cout << "Initial status:\n";
    printStatus();
    cout << "\n";

    startTime = chrono::steady_clock::now();
    mt19937 rng(randomSeed());
    uniform_int_distribution<int> randomTime(minClearTime, maxClearTime);

    // if a party can't start, stop sending parties in but let the ones already inside finish
    int partiesStarted = 0;
    exception_ptr startError;
    try {
        for (int p = 1; p <= parties; p++) {
            enterDungeon(p, randomTime(rng));
            partiesStarted++;
        }
    } catch (...) {
        startError = current_exception();
    }
    // every thread has to be joined before main returns, even after an error
    for (int i = 0; i < maxInstances; i++) {
        if (runs[i].joinable()) runs[i].join();
    }

    cout << "\nFinal status:\n";
    printStatus();

    int totalParties = 0;
    long long totalTime = 0;
    cout << "\n=== Summary ===\n";
    for (int i = 0; i < maxInstances; i++) {
        cout << "Instance " << i + 1 << ": parties served = " << partiesServed[i] << ", total time served = "
             << timeServed[i] << "s\n";
        totalParties += partiesServed[i];
        totalTime += timeServed[i];
    }
    cout << "Total parties served: " << totalParties << "\n";
    cout << "Total time served: " << totalTime << "s\n";
    cout << "Leftover players: " << leftTanks << " tank(s), " << leftHealers << " healer(s), " << leftDps << " DPS\n";
    cout << "Elapsed time: " << fixed << setprecision(1) << elapsed() << "s\n";

    bool failed = false;
    if (startError) {
        cerr << "\nError: Party " << partiesStarted + 1 << " could not start (" << errorMessage(startError) << "), "
             << parties - partiesStarted << " of " << parties << " parties were not served\n";
        failed = true;
    }
    if (runError) {
        cerr << "\nError during a dungeon run: " << errorMessage(runError) << "\n";
        failed = true;
    }

    // cout doesn't throw when a write fails (e.g. redirected to a full disk), so check it here
    cout.flush();
    if (!cout) {
        cerr << "Error: could not write all of the output\n";
        failed = true;
    }
    return failed ? 1 : 0;
}
