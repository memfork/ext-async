# Async-IO Client API

Coroutine style API became the mainstream in the Swoole `core` since version 4.3.0.

`ext-async` depends on swoole, it is a extension of swoole, including the async callback style API.

You have to install Swoole extension before installing this extenion.

### Major change since version 4.3.0

Async clients and API are moved to a separate PHP extension `swoole_async` since version 4.3.0, install `swoole_async`:

```shell
git clone https://github.com/swoole/async-ext.git
cd async-src
phpize
./confiugre
make -j 4
sudo make install
```

Enable it by adding a new line `extension=swoole_async.so` to `php.ini`.
