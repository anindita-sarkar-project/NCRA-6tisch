# NCRA-6tisch
Delay-based non-cooperative rate adaptation for congestion control in 6TiSCH networks. Contiki-NG implementation on top of the ALICE and A3 autonomous schedulers, using TSCH ASN for one-way delay measurement and EB frames for per-subtree congestion pricing.

Steps to implement our approach:
1. git clone https://github.com/contiki-ng/contiki-ng.git
2. cd contiki-ng
3. Copy the files from the GitHub repo to the cloned contiki-ng directory.
4. In the simple-node-ksh directory - Alice + NCRA implemented
5. And in the A3 directory - A3 + NCRA implemented
6. Core implementation files are in the os directory
