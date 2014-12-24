The files inside this directory can be used as init script.

To install:

    $ sudo cp init.d/picam /etc/init.d/
    $ sudo cp default/picam /etc/default/

Edit /etc/default/picam to match your environment.

To start picam automatically on boot:

    $ sudo update-rc.d picam defaults

To disable autostart:

    $ sudo update-rc.d picam remove

To start picam manually:

    $ sudo service picam start

To stop picam manually:

    $ sudo service picam stop
