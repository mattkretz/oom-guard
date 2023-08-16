# User-space out-of-memory guard

This program was born out of a frustration with the kernel OOM mechanism, which 
simply doesn't work. Even after letting the laptop burn for 1 hour, nothing 
came back to life and the out-of-memory situation simply persisted.

This simple program runs in user space and checks available memory in 
/proc/meminfo every second. It tries to do this as efficiently as possible to 
conserve power (the target device is a laptop that is often used on battery).

If the available memory drops below 750 MiB the program starts sending SIGTERM 
signals to PIDs with highest oom_score_adj. In addition, it will ask Akonadi to 
shut down (every time; just in case).

Also, since we're getting close to OOM, this program tries to avoid all heap 
allocations and uses local (locked) memory instead.

## How to use it

```sh
make exec
```

Or:
```sh
make
./oom-guard
```
