# READ ME: Linux CFS Simulation

# About:
This program is a simulation of the Linux Completely Fair Scheduler. 
It uses a single producer thread to create tasks by reading in a pre-configured 
.csv file for four consumer threads acting as cpu cores to "execute".

# How To Use:
Compile the program using the compile script in a terminal:

	$ ./compile

This script will produce a single executable file: cfs. Run this program
in a terminal:

	$ ./cfs

# Note: 
This program requires the csv file processes_to_run.csv that was included in
the submission. This csv file takes the exact same format as the example used
in the lab 11 manual, and can be configured (although it is already preconfigured).
