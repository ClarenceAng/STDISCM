#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
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
vector<int> timeServed;
vector<thread> runs;

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

void runDungeon(int instance, int party, int clearTime) {
    this_thread::sleep_for(chrono::seconds(clearTime));

    mtx.lock();
    status[instance] = "empty";
    partiesServed[instance]++;
    timeServed[instance] += clearTime;
    printEvent("Party " + to_string(party) + " cleared Instance " + to_string(instance + 1));
    mtx.unlock();

    freeInstances.release();
}

void enterDungeon(int party, int clearTime) {
    // wait for a free instance before locking, otherwise it can deadlock
    freeInstances.acquire();

    mtx.lock();
    int instance = pickInstance();
    status[instance] = "active";
    printEvent("Party " + to_string(party) + " entered Instance " + to_string(instance + 1) +
               " (clear time: " + to_string(clearTime) + "s)");
    mtx.unlock();

    if (runs[instance].joinable()) runs[instance].join();
    runs[instance] = thread(runDungeon, instance, party, clearTime);
}

int main(int argc, char* argv[]) {
    if (argc > 2) {
        cerr << "Usage: lfg [config file]   (the config file defaults to config.txt)\n";
        return 1;
    }
    string configFile = "config.txt";
    if (argc == 2) configFile = argv[1];

    loadConfig(configFile);

    long long parties = min({numTanks, numHealers, numDps / 3});
    long long leftTanks = numTanks - parties;
    long long leftHealers = numHealers - parties;
    long long leftDps = numDps - parties * 3;

    status.assign(maxInstances, "empty");
    partiesServed.assign(maxInstances, 0);
    timeServed.assign(maxInstances, 0);
    runs.resize(maxInstances);
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
    mt19937 rng(random_device{}());
    uniform_int_distribution<int> randomTime(minClearTime, maxClearTime);

    for (int p = 1; p <= parties; p++) {
        enterDungeon(p, randomTime(rng));
    }
    for (int i = 0; i < maxInstances; i++) {
        if (runs[i].joinable()) runs[i].join();
    }

    cout << "\nFinal status:\n";
    printStatus();

    int totalParties = 0;
    int totalTime = 0;
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
    return 0;
}
