## FileSync

FileSync is a client-server system that helps you synchronize files across devices.

### How FileSync Works

On server / client side, you need to specify a directory to be synced from / to, say *src* / *dst*.

FileSync will traverse *src* and check file modification time to decide whether a file should be synchronized to *dst*.

## Usage

### Compile

FileSync is dependent on [Clibrary](https://github.com/JasonHu1012/Clibrary), you should clone and compile it first, and note that Clibrary and FileSync should be put at the same directory.

Once finished, run `make all` to compile server and client programs.

### Server

`-d`: working directory, corresponding to `workDir` in config, default to be current working directory

`-p`: port, corresponding to `port` in config, default to be 52124

```bash
./server -d <dir> -p <port>
```

### Client

`--host`: host ip, corresponding to `host` in config, default to be localhost

`-p`: port, corresponding to `port` in config, default to be 52124

`--ldir`: local directory to be synced to, corresponding to `localDir` in config, default to be current working directory

`--rdir`: remote directory (in relative path) at server to be synced from, corresponding to `remoteDir` in config, default to be server working directory

```bash
./client --host <ip> -p <port> --ldir <ldir> --rdir <rdir>
```

#### Query Mode

In case you forget the server working directory, you may use

```bash
./client --query
```

to check it, and no files will be synchronized.