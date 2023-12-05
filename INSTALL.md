# Build and installation instructions

## Build dependencies

 - cmake (>= 3.13)
 - cmake-extras
 - glib-2.0 (>= 2.36)
 - gio-2.0 (>=2.36)
 - intltool
 - systemd
 - libayatana-common
 - accountsservice

## For end-users and packagers

```
cd ayatana-indicator-a11y
mkdir build
cd build
cmake ..
make
sudo make install
```

**The install prefix defaults to `/usr`, change it with `-DCMAKE_INSTALL_PREFIX=/some/path`**
