# Networked Producer-Consumer (Media Upload Service)

Simulates a media upload service. You put videos in one main folder, `videos/`. A **scheduler** hands each video to one of **p** producers by moving it into that producer's own subfolder. Each producer watches its folder and uploads whatever appears there over **gRPC** to a consumer. The consumer keeps uploads in a **leaky bucket queue** of length **q** and drops anything that doesn't fit. **c** consumer threads compress each video, cut a 10 second preview and save the result to a single folder. A browser GUI lists the uploaded videos, previews 10 seconds on hover and plays the full video on click.

The services run in Docker containers: one container per producer, and one consumer container running the c threads. All of them start with the service and run until it stops. The command line program and its scheduler run on the host. The containers stand in for the separate virtual machines in the specification, and they only talk to each other over gRPC.

The program is written in C++17. The only other code is the browser page (`consumer/frontend`) and the two short run scripts.

## Files

```
config.txt                  p, c, q and the optional settings
run.bat, run.sh             build and start the command line program
tools/upload_cli.cpp        the command line program and the scheduler: starts the containers, then moves new videos into the producers' folders
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

These are created when it runs and are not part of the source: `docker-compose.yml`, `videos/` (the main folder, with the producers' folders inside it while the service runs) and `storage/` (the saved results).

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

   It builds the images, creates `videos/` with a folder for each producer, starts the consumer and the producers, and then keeps running and shows what happens. Nothing is typed into it.

3. Open <http://localhost:8080>.
4. Copy or drag videos, or folders of videos, into `videos/`, at any time and as many as you like:

   ```
   videos/
     clip.mp4          put new videos here,
     trip/beach.mp4    or in folders here
     producer-1/       the scheduler moves them into these
     producer-2/
     producer-3/
   ```

5. Press **Ctrl+C once** to stop, and wait a few seconds while it clears up. The containers and the producers' folders are removed, as described under [The scheduler](#the-scheduler). The saved videos stay in `storage/`. To start again with an empty library, delete `storage/`.

   On Windows, `run.bat` then asks `Terminate batch job (Y/N)?`. Either answer is fine, because everything has already been stopped by then.

The script does these steps, which can also be run by hand:

```bat
g++ -std=c++17 -O2 -Wall -Wextra -pthread -static tools\upload_cli.cpp -o tools\upload_cli.exe
tools\upload_cli.exe config.txt docker-compose.yml
```

`-static` is for MinGW and is left out in `run.sh`. Inside the images, each program is built with CMake (`producer/backend/Dockerfile`, `consumer/backend/Dockerfile`).

## The scheduler

Every `scan_seconds`, the scheduler looks through `videos/` and the folders inside it. The producers' folders are skipped.

- A video is only taken once its size and modification time haven't changed since the previous look, so a file that is still being copied in is left alone.
- It is moved, not copied, into the folder of the running producer with the fewest videos waiting, and the window shows the move. A video counts as waiting from the moment it is moved until its producer has a result for it, so the one being sent counts too. A tie goes to the lowest producer number.
- Videos that arrive together are dealt out to producer-1, 2, 3, 1, 2, 3 in order. After that, a producer that is busy with a big file gets fewer new ones.
- If the producer's folder already has a file with that name, a number is added, as in `clip (2).mp4`. Nothing in a producer's folder is ever replaced.
- Videos are also moved out of folders inside `videos/`. The emptied folders are left where they are.

Videos stay in their producer's folder after their result. The producer prints a result line for each one, and the scheduler counts what is still waiting from those lines.

When you stop it, once the containers have stopped, the producers' folders are removed:

- **Deleted:** videos that got a result (uploaded, dropped, duplicate, rejected or failed).
- **Moved back into `videos/`, to be sent next time:** videos that were never sent, and uploads the consumer accepted but stopped before saving. Anything else in a producer's folder goes back too.
- If something can't be deleted or moved, for example because another program has it open, the window says so and that producer's folder is left in place.

If the program ends without clearing up, for example because its window is closed, the folders stay. At the next start each producer sends everything left in its folder again. Videos that were already saved come back as duplicates.

## Settings

Restart the script after changing `config.txt`. `docker-compose.yml` is written from it each time the service starts.

| Setting | Meaning | Allowed | Default |
| --- | --- | --- | --- |
| `p` | producers, each with its own folder | 1 to 64 | required |
| `c` | consumer threads | 1 to 64 | required |
| `q` | max queue length | 1 to 100000 | required |
| `http_port` | port for the GUI | 1 to 65535 | 8080 |
| `compress_height` | videos taller than this are scaled down | 144 to 2160 | 480 |
| `scan_seconds` | time between looks at the main folder and at each producer's folder | 1 to 3600 | 2 |
| `max_upload_mb` | largest video accepted | 1 to 102400 | 2048 |

## How a video moves through the system

1. The scheduler finds the video in `videos/`, waits until it stops changing and moves it into the folder of the producer with the fewest videos waiting.
2. That producer finds it, checks again that it isn't changing, hashes it and sends only a header (name, size, SHA-256) to the consumer.
3. The consumer checks the hash. A video it already has, or is already receiving, is answered with **DUPLICATE**.
4. It then tries to reserve a queue slot. If all q slots are taken the answer is **QUEUE_FULL** and the video is dropped.
5. Otherwise the answer is **ACCEPTED** and the producer streams the file in 1 MB chunks. The consumer hashes what arrives and rejects it if it doesn't match.
6. The finished upload waits in the queue until one of the c consumer threads takes it, which frees its slot.
7. The thread compresses the video to H.264 with OpenCV, copies the original audio back in with FFmpeg, cuts the preview and saves everything.

q counts uploads in progress plus uploads waiting for a thread. Videos a thread is already working on have left the queue. Videos waiting in `videos/` or in a producer's folder are separate from this queue. Nothing is dropped from them, and when you stop the service any that weren't sent are kept in `videos/`.

Each producer sends one video at a time, and all of them send to the same consumer. A producer's videos can be compressed by any of the c threads, whichever is free when the video reaches the front of the queue.

The window shows each move and each producer's result for each of its files. The GUI shows the queue, the totals and recent activity. A dropped video isn't retried, and it is deleted with its producer's folder when the service stops. To send it again, move it from the producer's folder back into `videos/` before you stop it.

## When something goes wrong

Every failure is reported with its reason in the window, in the GUI's recent activity, or in the consumer's log (`docker compose logs consumer`). Nothing is dropped without saying so.

| What happens | What the service does |
| --- | --- |
| Docker isn't running, Compose is missing, the build or a port fails | The window says which, and it removes anything it had started |
| `videos/` or a producer's folder can't be created or read at start-up | The window says which, and it stops |
| Ctrl+C can't be taken over at start-up, so the service couldn't be stopped cleanly | The window says so, and it doesn't start |
| The consumer stops while starting | The window shows its log, and it stops |
| The consumer stops later | The window shows its last messages, and the scheduler keeps new videos in `videos/` until you restart |
| A file in `videos/` isn't a supported video, is a link, is empty or has a control character in its name | The scheduler leaves it where it is and says why once |
| A video can't be moved, for example because another program has it open | The scheduler says why once and tries again on every look |
| A folder inside `videos/` can't be read | The scheduler says so once, carries on with the rest and tries again on every look |
| A producer stops | The window shows its exit code, and the scheduler gives its share to the others. What was waiting in its folder goes back into `videos/` when the service stops |
| A producer's folder can't be read 5 times in a row | That producer stops, as above |
| A video is taken out of a producer's folder before it is sent | The producer reports it as failed, and the scheduler stops counting it as waiting |
| A file in a producer's folder isn't a supported video | The producer logs it once and carries on with the rest |
| A file can't be read, or the connection drops | The producer tries that file 5 times, then reports it as failed and carries on with the rest |
| The consumer can't be reached | Producers keep trying and say so every minute |
| A run ended without clearing up and `p` was lowered since, so a producer folder past it still holds videos | The window says so at start-up. No producer reads that folder, and when the service stops its videos go back into `videos/` and the folder is removed |
| A producer or the consumer freezes or loses its network | Connection pings notice within about 30 seconds, and the upload is cancelled so its queue slot comes back |
| A transfer stays slower than 256 KB/s on average | The deadline cancels it, so it can't hold a queue slot for ever |
| The consumer's temp disk fills during an upload | The producer is told REJECTED with the reason, part way through |
| A file isn't really a video | It is reported as failed, and it can be sent again |
| Part of a video is damaged | What could be decoded is saved, and the activity entry says how much |
| Its audio can't be copied or stops early | The video is saved anyway, and the activity entry says what happened |
| The disk fills while saving | The files already written for that video are removed, so nothing half-saved is left, and it can be sent again |
| You stop the service with accepted videos unfinished | Each one is named in the window as abandoned and goes back into `videos/`, to be sent next time |
| A file in a producer's folder can't be deleted or moved when the service stops | The window says which and why, and that producer's folder is left in place |
| The consumer's log can't be read when the service stops | Uploaded videos go back into `videos/` too, because none of them can be confirmed as saved. Next time the saved ones come back as duplicates |
| The program ends without clearing up, for example its window is closed | The producers' folders stay, and each producer sends what is in its folder again at the next start |
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

- Videos are moved, not copied, and the ones that got a result are deleted when the service stops. Keep your own copy elsewhere.
- Audio is copied, not converted, so only AAC, MP3 and Opus are kept. A video with any other audio is saved without sound and marked "no audio" in the GUI.
- A video with a variable frame rate is written at a constant rate, so its sound can drift slightly from the picture.
- Compressing a video that isn't H.264 can make it larger, because it still has to be converted for browsers to play it.
- Docker Desktop runs all of the containers inside one virtual machine on the host.
