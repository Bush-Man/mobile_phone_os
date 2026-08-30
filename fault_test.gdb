set pagination off
set confirm off
file build/kernel.elf
target remote :1247
break *0x40000c00
continue
printf "\n=== HIT (fault context) ===\n"
printf "--- 1. identity reads of ipcdemo table chain (VA=PA) ---\n"
printf "root L0[2] @0x478e1010 (expect 478e5003):\n"
x/1gx 0x478e1010
printf "L1[0] @0x478e5000 (expect 478f2003):\n"
x/1gx 0x478e5000
printf "L2[0] @0x478f2000 (expect 478f4003):\n"
x/1gx 0x478f2000
printf "L3[0] @0x478f4000 (expect 00200000478e3fc1):\n"
x/1gx 0x478f4000
printf "leaf pa bytes @0x478e3000 (init text? no - ipcdemo text):\n"
x/4xb 0x478e3000
printf "--- 2. data-side read of user VA 0x1000000002c (LAST, may fail) ---\n"
x/4xb 0x1000000002c
detach
quit
