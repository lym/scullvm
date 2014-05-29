# scullvm
A trivial character driver module that implement virtual addresses. This
module allocates 16 pages at time. This is done to improve system
performance.

## Testing
To really comprehend the advantage of this module over others that
employ other allocation techniques, one needs to write and copy
sizable files while benchmarking the driver against others.
The snippets below are just tests to show that the driver actually
works and do not show the advantage it possesses.

    make

    sudo insmod scullpg.ko

    sudo chmod g+rw /dev/scull_pg

    sudo chmod o+rw /dev/scull_pg

    echo -n "BienVenue" > /dev/scull_pg

    dd bs=20 count=5 if=/dev/scull_pg of=~/scullpgout.txt

    vim /home/lym/scullpgout.txt

## Disclaimer
You might wanna try this code on a computer that doesn't have important
data. As data losses are sooooo possible.
