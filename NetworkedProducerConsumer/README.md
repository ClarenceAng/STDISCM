# Networked Producer-Consumer (Media Upload Service)

Simulates a media upload service. You type video or folder paths at an always-open prompt, and up to **p** producers at a time upload them over **gRPC** to a consumer. The consumer keeps uploads in a **leaky bucket queue** of length **q**, drops anything that doesn't fit, and **c** consumer threads compress each video, cut a 10 second preview and save the result to a single folder. A browser GUI lists the uploaded videos, previews 10 seconds on hover and plays the full video on click.

The services run in Docker containers: one container per producer, started when there is a path for it, and one consumer container running the c threads. The prompt runs on the host. The containers stand in for the separate virtual machines in the specification, and they only talk to each other over gRPC.

The program is written in C++17. The only other code is the browser page (`consumer/frontend`) and the two short run scripts.

## Files

```
config.txt                  p, c, q and the optional settings
run.bat, run.sh             build and start the command line program
tools/upload_cli.cpp        the prompt: starts the consumer, then one producer per path, rewriting docker-compose.yml as it goes
proto/media_upload.proto    the gRPC contract between producer and consumer
common/                     SHA-256 helper, the video file types, error messages and the CMake rule for the gRPC code
producer/backend/           the producer
consumer/backend/           the consumer
  leaky_bucket_queue.h        the bounded queue
  dedup_index.h               SHA-256 duplicate detection
  upload_service.cpp          gRPC handler, the producer side of the queue
  video_worker.cpp            the c consumer threads, OpenCV compression and the preview
  av_mux.cpp                  copies the original audio into the compressed video (FFmpeg)
  video_library.cpp           the list of saved videos
  web_server.cpp              serves the GUI, its API and the video files
consumer/frontend/          the browser GUI
```

These are created when it runs and are not part of the source: `docker-compose.yml` and `storage/` (the saved results).

## Building and running

You need:

- **Docker** with Compose v2. On Windows, Docker Desktop must be running.
- **g++** with C++17 support on the PATH, which is only used for the small command line program. Everything else is compiled inside the Docker images, so gRPC, OpenCV and FFmpeg don't need to be installed.
- An internet connection for the first build, which takes several minutes.

1. Set `p`, `c` and `q` in `config.txt`.
2. From this folder, run

   ```bat
   .\run.bat
   ```

   or on Linux / macOS

   ```sh
   sh run.sh
   ```

   It builds the images, starts the consumer and then stays open at an `upload>` prompt.

3. Open <http://localhost:8080>.
4. Type the path of a video or a folder of videos and press Enter, one path per line, for as long as you like:

   ```
   upload> C:\clips\folder
   upload> C:\clips\my video.mp4
   ```

5. Type `exit` to stop. The containers are removed and the saved videos stay in `storage/`. To start again with an empty library, delete that folder.

Each path starts one producer, which uploads the videos in that folder, or just that one video and not the files next to it, and then finishes. Producers only read these paths and never change them. At most `p` run at once. A path typed while all of them are busy waits at the prompt and starts when one is free, in the order typed. Typing a path again sends it again, and anything already saved comes back as a duplicate.

Paths can be absolute or relative to the folder your terminal was in when you started the script. Quotes around a path are fine but not needed. Paths can also be given when starting, as in `.\run.bat C:\clips\a C:\clips\b`, and they are handled as if they were typed first.

The script does these steps, which can also be run by hand:

```bat
g++ -std=c++17 -O2 -Wall -Wextra -pthread -static tools\upload_cli.cpp -o tools\upload_cli.exe
tools\upload_cli.exe config.txt docker-compose.yml
```

`-static` is for MinGW and is left out in `run.sh`. Inside the images, each program is built with CMake (`producer/backend/Dockerfile`, `consumer/backend/Dockerfile`).

## Settings

Restart the script after changing `config.txt`. `docker-compose.yml` is rewritten from it as producers start, so its contents change while the program runs.

| Setting | Meaning | Allowed | Default |
| --- | --- | --- | --- |
| `p` | most producers running at once | 1 to 64 | required |
| `c` | consumer threads | 1 to 64 | required |
| `q` | max queue length | 1 to 100000 | required |
| `http_port` | port for the GUI | 1 to 65535 | 8080 |
| `compress_height` | videos taller than this are scaled down | 144 to 2160 | 480 |
| `scan_seconds` | time a producer waits between looks at its path | 1 to 3600 | 2 |
| `max_upload_mb` | largest video accepted | 1 to 102400 | 2048 |

## How a video moves through the system

1. The producer finds each video at its path and waits until its size and modification time stop changing between two scans, so a file that is still being copied in isn't sent.
2. It hashes the file and sends only a header (name, size, SHA-256) to the consumer.
3. The consumer checks the hash. A video it already has, or is already receiving, is answered with **DUPLICATE**.
4. It then tries to reserve a queue slot. If all q slots are taken the answer is **QUEUE_FULL** and the video is dropped.
5. Otherwise the answer is **ACCEPTED** and the producer streams the file in 1 MB chunks. The consumer hashes what arrives and rejects it if it doesn't match.
6. The finished upload waits in the queue until one of the c consumer threads takes it, which frees its slot.
7. The thread compresses the video to H.264 with OpenCV, copies the original audio back in with FFmpeg, cuts the preview and saves everything.

q counts uploads in progress plus uploads waiting for a thread. Videos a thread is already working on have left the queue. Paths waiting at the prompt are separate from this queue: nothing is dropped from them, and any still waiting when you type `exit` are not uploaded.

The prompt shows each producer's result for each of its files, and the GUI shows the queue, the totals and recent activity. A dropped video is not retried, so type its path again to resend it.

## When something goes wrong

Every failure is reported with its reason at the prompt, in the GUI's recent activity, or in the consumer's log (`docker compose logs consumer`). Nothing is dropped without saying so.

| What happens | What the service does |
| --- | --- |
| Docker isn't running, Compose is missing, the build or a port fails | The prompt says which, and removes anything it had started |
| The consumer stops while starting | The prompt shows its log and exits |
| The consumer stops later | The prompt shows its last messages and refuses new paths until you restart |
| A path doesn't exist, can't be read or isn't a video | The prompt says so and keeps running |
| A file in a folder isn't a supported video | The producer logs it once and carries on with the rest |
| A file can't be read, or the connection drops | The producer tries that file 5 times, then reports it as failed and exits with code 1, which the prompt shows |
| The consumer can't be reached for 2 minutes | The producer gives up and exits with code 1 |
| A producer or the consumer freezes or loses its network | Connection pings notice within about 30 seconds, and the upload is cancelled so its queue slot comes back |
| A transfer stays slower than 256 KB/s on average | The deadline cancels it, so it can't hold a queue slot for ever |
| The consumer's temp disk fills during an upload | The producer is told REJECTED with the reason, part way through |
| A file isn't really a video | It is reported as failed, and it can be sent again |
| Part of a video is damaged | What could be decoded is saved, and the activity entry says how much |
| Its audio can't be copied or stops early | The video is saved anyway, and the activity entry says what happened |
| The disk fills while saving | The files already written for that video are removed, so nothing half-saved is left, and it can be sent again |
| You stop the service with accepted videos unfinished | Each one is named at the prompt as abandoned. Send them again after restarting |
| `manifest.jsonl` has a bad line, or a listed video was deleted | That line is skipped and logged, and the rest of the library loads |
| `manifest.jsonl` exists but can't be read | The consumer refuses to start, because otherwise every saved video would look new |
| A video, preview or poster is missing when requested | The GUI shows what is unavailable, and the consumer logs it |

## Bonus features

- **Queue full notice:** the consumer tells the producer before any of the video is sent, and the producer logs it as `DROPPED`.
- **Duplicate detection:** by SHA-256. It also catches the same video arriving from two producers at once, and it still works after a restart because saved videos are listed in `storage/manifest.jsonl`.
- **Video compression:** OpenCV re-encodes to H.264 and scales down to `compress_height`. If that doesn't make the file smaller and the original can be played by browsers as it is, the original video stream is kept.

## Where the results go

- `storage/videos/`: the saved videos, all in one folder
- `storage/previews/`: the 10 second previews and the poster images
- `storage/manifest.jsonl`: the list of saved videos

## Limitations

- Audio is copied, not converted, so only AAC, MP3 and Opus are kept. A video with any other audio is saved without sound and marked "no audio" in the GUI.
- A video with a variable frame rate is written at a constant rate, so its sound can drift slightly from the picture.
- Compressing a video that isn't H.264 can make it larger, because it still has to be converted for browsers to play it.
- Docker Desktop runs all of the containers inside one virtual machine on the host.
