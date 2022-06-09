mpicc word-count-printer.c -lm
mpirun --allow-run-as-root -np 1 ./a.out "output1.csv"
diff oracle.csv output1.csv
mpirun --allow-run-as-root -np 2 ./a.out "output2.csv"
diff oracle.csv output2.csv
mpirun --allow-run-as-root -np 3 ./a.out "output3.csv"
diff oracle.csv output3.csv
mpirun --allow-run-as-root -np 4 ./a.out "output4.csv"
diff oracle.csv output4.csv
mpirun --allow-run-as-root -np 5 ./a.out "output5.csv"
diff oracle.csv output5.csv
mpirun --allow-run-as-root -np 6 ./a.out "output6.csv"
diff oracle.csv output6.csv
rm output1.csv
rm output2.csv
rm output3.csv
rm output4.csv
rm output5.csv
rm output6.csv
rm a.out