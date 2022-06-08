#include <ctype.h>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

#include "mpi.h"

//----------------------------------------------DICHIARAZIONE COSTANTI----------------------------------------------//
#define INITIAL_CAPACITY 16  // Capacità della HashTable iniziale (non può essere 0)
#define FNV_OFFSET 14695981039346656037UL //Costante per funzione di Hashing
#define FNV_PRIME 1099511628211UL //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
#define SIZE 4096 //Serve per la massima lunghezza di una riga in un file
#define FILE_NAME 30 //Serve per la massima lunghezza di una nome di un file
#define WORD_LENGTH 20 //Serve per la massima lunghezza di una parola della hashtable
#define MASTER 0 //Processo Master MPI
#define NUMBER_OF_FILES 40 //Numero massimo di files (viene usato solo all'inizio perchè poi il tutto è gestito dinamicamente)
#define DIRECTORY_SIZE 200 //Lunghezza massima della directory

typedef struct ht_entry HashTableEntry;
typedef struct ht HashTable;
typedef struct hti HashTableIterator;
typedef struct dataDist DataDistribution;
typedef struct merged_ht MergedHashTable;
//-------------------------------------------------STRUTTURE DATI---------------------------------------------------//
// Hash table entry (slot may be filled or empty).
struct ht_entry{
    const char* key;  // key is NULL if this slot is empty
    void* value;
};
// Hash table structure: create with ht_create, free with ht_destroy.
struct ht {
    HashTableEntry* entries;  // hash slots
    size_t capacity;    // size of _entries array
    size_t length;      // number of items in hash table
};
struct hti{
    const char* key;  // current key
    void* value;      // current value

    // Don't use these fields directly.
    HashTable* _table;       // reference to hash table being iterated
    size_t _index;    // current index into ht._entries
};
//Struttura dati per distribuzione del lavoro
struct dataDist{
    char fileNamesProcess[NUMBER_OF_FILES * FILE_NAME]; // Nomi dei file che ogni processo deve leggere
    int sizeFilesProcess[NUMBER_OF_FILES]; // Size dei file che ogni processo deve leggere
    int size;   // # dei file che ogni processo deve leggere
    int offset; // Corrisponde al byte dal quale il processo i-esimo deve iniziare a leggere (tranne processo MASTER)
    int amount; // # di caratteri che ogni processo deve leggere
};
// Struttura dati per ordinamento
struct merged_ht{
    char word[WORD_LENGTH];
    int frequencies;
    int numEntries;
};


//--------------------------------------------------FIRME METODI---------------------------------------------------//
HashTable* ht_create(void);
void ht_destroy(HashTable* table);
static uint64_t hash_key(const char* key);
void* ht_get(HashTable* table, const char* key);
static const char* ht_set_entry(HashTableEntry* entries, size_t capacity, const char* key, void* value, size_t* plength);
static bool ht_expand(HashTable* table);
const char* ht_set(HashTable* table, const char* key, void* value);
size_t ht_length(HashTable* table);
HashTableIterator ht_iterator(HashTable* table);
bool ht_next(HashTableIterator* it);
void exit_nomem(void);
void distributeData(DataDistribution * dataDistTemp, int nproc, int totalSize, int files, int sizeFileName, char * fileNames, int * sizeFiles);
int isChar(char c);
void insertWord(HashTable* counts, char *temp, int countWord);
//FUNZIONI DI MERGE
MergedHashTable* merged_ht_create(int numEntries);
void merged_ht_destroy(MergedHashTable* table);
void merge(MergedHashTable* mergedtable, int l, int m, int r);
void mergeSort(MergedHashTable* mergedTable, int l, int r);

//------------------------------------------------------MAIN-------------------------------------------------------//
int main(int argc, char * argv[]) {
    int my_rank, nproc;
    //--INIZIALIZZAZIONE MPI--//
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    // Hash Table che conterrà le coppie chiave-valore di ogni parola
    HashTable* counts = ht_create();

    // Struct che conterrà l'amount di lavoro che dovrà svolgere ogni processo
    DataDistribution dataDist;

    if (counts == NULL)
        exit_nomem();

    //Struttura dati che serve a prendere i dati di un file;
    struct stat fileStats;
    DIR *FD;
    FILE *fp;
    struct dirent *in_file;
    int files = 0;

    int sizeBuffer = SIZE;
    int filesLimit = NUMBER_OF_FILES;
    int sizeFileName = FILE_NAME;
    int directorySize = DIRECTORY_SIZE;

    char * buffer = malloc(sizeBuffer * sizeof(char) + 1); //+1 perchè c'è '\0'
    int totalSize = 0;

    //Current Working Directory, buffer statico
    char cwd[directorySize];

    //--VARIABILI MPI--//
    double start, end;
    MPI_Request request = MPI_REQUEST_NULL;
    MPI_Status status;

    // Creo il datatype per la distribuzione dei dati
    MPI_Datatype MPI_DATA_DISTRIBUTION;
    int lengths[5] = {filesLimit * sizeFileName, filesLimit, 1, 1, 1};
    MPI_Datatype types[5] = {MPI_CHAR, MPI_INT, MPI_INT, MPI_INT, MPI_INT};
    MPI_Aint base_address, displacements[5];
    DataDistribution dummyDataDistribution;

    MPI_Get_address(&dummyDataDistribution, &base_address);
    MPI_Get_address(&dummyDataDistribution.fileNamesProcess, &displacements[0]);
    MPI_Get_address(&dummyDataDistribution.sizeFilesProcess, &displacements[1]);
    MPI_Get_address(&dummyDataDistribution.size, &displacements[2]);
    MPI_Get_address(&dummyDataDistribution.offset, &displacements[3]);
    MPI_Get_address(&dummyDataDistribution.amount, &displacements[4]);

    for(int i = 0; i < 5; i++)
        displacements[i] = MPI_Aint_diff(displacements[i], base_address);

    MPI_Type_create_struct(5, lengths, displacements, types, &MPI_DATA_DISTRIBUTION);
    MPI_Type_commit(&MPI_DATA_DISTRIBUTION);

    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);

    //Inizio a contare il tempo
    start = MPI_Wtime();

    //Prendiamo la directory della cartella contenente i file
    strcat(getcwd(cwd, sizeof(cwd)), "/fileList");

    if(my_rank == MASTER){
        if (!(FD = opendir(cwd))) {
            fprintf(stderr, "Error : Failed to open input directory - %s\n", strerror(errno));
            return 1;
        }

        char * fileNames = malloc(filesLimit * sizeFileName * sizeof(char));
        int * sizeFiles = malloc(filesLimit * sizeof(int));

        while ((in_file = readdir(FD)) != NULL) 
            if(strcmp(in_file->d_name, ".") && strcmp(in_file->d_name, "..")){
                files++;

                //Inserisco il path della sottocartella contenente i file
                char pathFile[DIRECTORY_SIZE] = "fileList/"; 
                strcat(pathFile,in_file->d_name);

                if (stat(pathFile, &fileStats)){
                    printf("Unable to get file properties.\n");
                    printf("Please check whether '%s' file exists.\n\n", in_file->d_name);
                }

                if(files > filesLimit){
                    filesLimit *= 2;
                    fileNames = realloc(fileNames, filesLimit * sizeFileName * sizeof(char));
                    sizeFiles = realloc(sizeFiles, filesLimit * sizeof(int));
                }

                strcpy(&fileNames[(files-1) * sizeFileName], strcat(in_file->d_name, "\0"));
                sizeFiles[files-1] = fileStats.st_size;
                totalSize += fileStats.st_size;
            }

        closedir(FD);

        fileNames = realloc(fileNames, files * sizeFileName * sizeof(char));
        sizeFiles = realloc(sizeFiles, files * sizeof(int));

        DataDistribution dataDistTemp[nproc];

        // Funzione che calcola il carico di lavoro che ogni processo deve svolgere
        distributeData(dataDistTemp,nproc,totalSize,files,sizeFileName,fileNames,sizeFiles);

        // Distribuisco i dati calcolati in sequenziale dal processo MASTER
        MPI_Scatter(&dataDistTemp[0], 1, MPI_DATA_DISTRIBUTION, &dataDist, 1, MPI_DATA_DISTRIBUTION, MASTER, MPI_COMM_WORLD);

        free(fileNames);
        free(sizeFiles);
    } else // Fine Master
        MPI_Scatter(NULL, 1, MPI_DATA_DISTRIBUTION, &dataDist, 1, MPI_DATA_DISTRIBUTION, MASTER, MPI_COMM_WORLD);

    int c_read = 0;
    int finished = 0;

    //Effettuo la lettura dei file in parallelo
    for(int i = 0; i < dataDist.size; i++){
        strcpy(buffer,"");

        // printf("[%s] - [%d]\n", &dataDist.fileNamesProcess[i * sizeFileName], dataDist.sizeFilesProcess[i]);

        char wordToAdd[SIZE];
        int j = 0;

        strcat(getcwd(cwd, sizeof(cwd)), "/fileList/");
        strcat(cwd,&dataDist.fileNamesProcess[i*sizeFileName]);

        // Si apre il file
        if (!(fp = fopen(cwd, "r"))) {
            fprintf(stderr, "Error: Failed to open entry file - %s\n", strerror(errno));
            fclose(fp);
            return 1;
        }

        if(my_rank != MASTER && i == 0){
            if(dataDist.offset != 0){
                fseek(fp, dataDist.offset - 1, SEEK_SET);
                if(isChar(fgetc(fp))){
                    while(isChar(fgetc(fp)))
                        c_read++;
                    c_read++;
                }
            }
        }

        while ((fgets(buffer, SIZE, fp)) != NULL && !finished) {
            for(int i=0; i < strlen(buffer); i++){
                if(++c_read == dataDist.amount){
                    finished = 1;

                    if(!isChar(buffer[i])){
                        wordToAdd[j] = '\0';
                        if(strlen(wordToAdd) > 0)
                            insertWord(counts, wordToAdd, 1);
                        strcpy(wordToAdd,"");
                        break;
                    }
                }

                if(isChar(buffer[i])){
                    wordToAdd[j++] = buffer[i];
                } else {
                    wordToAdd[j] = '\0';
                    
                    if(strlen(wordToAdd) > 0)
                        insertWord(counts, wordToAdd, 1);
                        
                    strcpy(wordToAdd,"");
                    j = 0;

                    if (finished)
                        break;
                }
            }
            if(ftell(fp) == dataDist.sizeFilesProcess[i]){
                if(strlen(wordToAdd) > 0)
                    insertWord(counts, wordToAdd, 1);
                    
                strcpy(wordToAdd,"");
                j = 0;
            }
        }
    }
    
    //Per MPI_Pack
    int wordLength = WORD_LENGTH;

    int sizeDataToPack = sizeof(int) + (((sizeof(char) * wordLength) + sizeof(int)) * counts->length);
    int countDataToPack = 4 + ((wordLength + 4) * counts->length);
    int position = 0;
    char * sendHashTable = malloc(sizeDataToPack);
    int sizeDataToReceive;
    HashTableIterator it = ht_iterator(counts);

    //Per Gatherv
    int numElement = counts->length * (wordLength + 4) + 4;
    int * countsElement;
    int * displacementsElement;
    char * htsReceived;

    if(my_rank == MASTER){
        countsElement = malloc(nproc * sizeof(int));
        displacementsElement = malloc(nproc * sizeof(int));   
    }

    MPI_Gather(&numElement, 1, MPI_INT, countsElement, 1, MPI_INT, MASTER, MPI_COMM_WORLD);
    
    if(my_rank == MASTER){
        int temp = 0;
        for(int i = 0; i < nproc; i++){
            if(i != 0){
                displacementsElement[i] = temp + countsElement[i-1];
                temp += countsElement[i-1];
            } else {
                displacementsElement[i] = 0;
            }
            sizeDataToReceive += countsElement[i];
        }
        htsReceived = malloc(sizeDataToReceive + 1);
    }

    // I dati vengono inpacchettati
    if(my_rank != MASTER){
        int length = counts->length;
        MPI_Pack(&length, 1, MPI_INT, sendHashTable, sizeDataToPack, &position, MPI_COMM_WORLD);
        for(int i = 0; i < length; i++){
            ht_next(&it);
            int countWord = *((int*) it.value);
            char key[wordLength];
            strcpy(key, it.key);

            MPI_Pack(key, wordLength, MPI_CHAR, sendHashTable, sizeDataToPack, &position, MPI_COMM_WORLD);
            MPI_Pack(&countWord, 1, MPI_INT, sendHashTable, sizeDataToPack, &position, MPI_COMM_WORLD);
        }
    }

    MPI_Gatherv(sendHashTable, countDataToPack, MPI_PACKED, htsReceived, countsElement, displacementsElement, MPI_PACKED, MASTER, MPI_COMM_WORLD);   

    if(my_rank == MASTER){
        int numEntries;
        for(int i = 1; i < nproc; i++){
            position = 0;
            MPI_Unpack(&htsReceived[displacementsElement[i]], countsElement[i], &position, &numEntries, 1, MPI_INT, MPI_COMM_WORLD);
            int countWord;
            char temp[wordLength];
            for(int j = 0; j < numEntries; j++){
                MPI_Unpack(&htsReceived[displacementsElement[i]], countsElement[i], &position, temp, wordLength, MPI_CHAR, MPI_COMM_WORLD);
                MPI_Unpack(&htsReceived[displacementsElement[i]], countsElement[i], &position, &countWord, 1, MPI_INT, MPI_COMM_WORLD);
                insertWord(counts,temp,countWord);
            }
        }
        // Ordinamento per ordine di frequenze e a parità di frequenze in ordine alfabetico
        numEntries = counts->length;
        MergedHashTable* mergedTable = merged_ht_create(numEntries);

        if (mergedTable == NULL)
            exit_nomem();

        // Struttura dati che permettere di scorrere gli elementi della Hash Table
        HashTableIterator hti = ht_iterator(counts);
        int i = 0;

        // Riempio gli array per riordinarli (per evitare di toccare la HashTable)
        while (ht_next(&hti)) {
            strcpy(mergedTable[i].word, hti.key);
            mergedTable[i].frequencies = *(int*) hti.value;
            i++;
        }

        // Effettuo l'ordinamento
        mergeSort(mergedTable, 0, numEntries - 1);

        merged_ht_destroy(mergedTable);
    }

    // Finisco di contare il tempo
    end = MPI_Wtime() - start;

    if(my_rank == MASTER){
        printf("\n~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\nTempo calcolo locale: % lf\n~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n\n", end);
    }

    ht_destroy(counts);

    free(buffer);

    MPI_Finalize();
    return 0;
}

//-----------------------------------------------------METODI------------------------------------------------------//
HashTable* ht_create(void) {
    // Allocate space for hash table struct.
    HashTable* table = malloc(sizeof(HashTable));
    if (table == NULL) {
        return NULL;
    }
    table->length = 0;
    table->capacity = INITIAL_CAPACITY;

    // Allocate (zero'd) space for entry buckets.
    table->entries = calloc(table->capacity, sizeof(HashTableEntry));
    if (table->entries == NULL) {
        free(table); // error, free table before we return!
        return NULL;
    }
    return table;
}
void ht_destroy(HashTable* table) {
    // First free allocated keys.
    for (size_t i = 0; i < table->capacity; i++) {
        if (table->entries[i].key != NULL) {
            free((void*)table->entries[i].key);
        }
    }

    // Then free entries array and table itself.
    free(table->entries);
    free(table);
}
// Return 64-bit FNV-1a hash for key (NUL-terminated). See description:
// https://en.wikipedia.org/wiki/Fowler–Noll–Vo_hash_function
static uint64_t hash_key(const char* key) {
    uint64_t hash = FNV_OFFSET;
    for (const char* p = key; *p; p++) {
        hash ^= (uint64_t)(unsigned char)(*p);
        hash *= FNV_PRIME;
    }
    return hash;
}
void* ht_get(HashTable* table, const char* key) {
    // AND hash with capacity-1 to ensure it's within entries array.
    uint64_t hash = hash_key(key);
    size_t index = (size_t)(hash & (uint64_t)(table->capacity - 1));

    // Loop till we find an empty entry.
    while (table->entries[index].key != NULL) {
        if (!strcmp(key, table->entries[index].key)) {
            // Found key, return value.
            return table->entries[index].value;
        }
        // Key wasn't in this slot, move to next (linear probing).
        index++;
        if (index >= table->capacity) {
            // At end of entries array, wrap around.
            index = 0;
        }
    }
    return NULL;
}
// Internal function to set an entry (without expanding table).
static const char* ht_set_entry(HashTableEntry* entries, size_t capacity, const char* key, void* value, size_t* plength) {
    // AND hash with capacity-1 to ensure it's within entries array.
    uint64_t hash = hash_key(key);
    size_t index = (size_t)(hash & (uint64_t)(capacity - 1));

    // Loop till we find an empty entry.
    while (entries[index].key != NULL) {
        if (!strcmp(key, entries[index].key)) {
            // Found key (it already exists), update value.
            entries[index].value = value;
            return entries[index].key;
        }
        // Key wasn't in this slot, move to next (linear probing).
        index++;
        if (index >= capacity) {
            // At end of entries array, wrap around.
            index = 0;
        }
    }

    // Didn't find key, allocate+copy if needed, then insert it.
    if (plength != NULL) {
        key = strdup(key);
        if (key == NULL) {
            return NULL;
        }
        (*plength)++;
    }
    entries[index].key = (char*)key;
    entries[index].value = value;
    return key;
}
// Expand hash table to twice its current size. Return true on success,
// false if out of memory.
static bool ht_expand(HashTable* table) {
    // Allocate new entries array.
    size_t new_capacity = table->capacity * 2;
    if (new_capacity < table->capacity) {
        return false;  // overflow (capacity would be too big)
    }
    HashTableEntry* new_entries = calloc(new_capacity, sizeof(HashTableEntry));
    if (new_entries == NULL) {
        return false;
    }

    // Iterate entries, move all non-empty ones to new table's entries.
    for (size_t i = 0; i < table->capacity; i++) {
        HashTableEntry entry = table->entries[i];
        if (entry.key != NULL) {
            ht_set_entry(new_entries, new_capacity, entry.key, entry.value, NULL);
        }
    }

    // Free old entries array and update this table's details.
    free(table->entries);
    table->entries = new_entries;
    table->capacity = new_capacity;
    return true;
}
const char* ht_set(HashTable* table, const char* key, void* value) {
    assert(value != NULL);
    if (value == NULL) {
        return NULL;
    }

    // If length will exceed half of current capacity, expand it.
    if (table->length >= table->capacity / 2) {
        if (!ht_expand(table)) {
            return NULL;
        }
    }

    // Set entry and update length.
    return ht_set_entry(table->entries, table->capacity, key, value, &table->length);
}
size_t ht_length(HashTable* table) {
    return table->length;
}
HashTableIterator ht_iterator(HashTable* table) {
    HashTableIterator it;
    it._table = table;
    it._index = 0;
    return it;
}
bool ht_next(HashTableIterator* it) {
    // Loop till we've hit end of entries array.
    HashTable* table = it->_table;
    while (it->_index < table->capacity) {
        size_t i = it->_index;
        it->_index++;
        if (table->entries[i].key != NULL) {
            // Found next non-empty item, update iterator key and value.
            HashTableEntry entry = table->entries[i];
            it->key = entry.key;
            it->value = entry.value;
            return true;
        }
    }
    return false;
}
void exit_nomem(void) {
    fprintf(stderr, "out of memory\n");
    exit(1);
}
void distributeData(DataDistribution * dataDistTemp, int nproc, int totalSize, int files, int sizeFileName, char * fileNames, int * sizeFiles){
    int sizeBlocks[nproc];
    int sizeBlockLocal;

    for(int i = 0; i < nproc; i++)
        sizeBlocks[i] = floor((double)totalSize/nproc);

    double rest = ((double)totalSize/nproc) - (totalSize/nproc);
    int bytesInExcess = 0;

    if(rest != 0){
        bytesInExcess = ceil(rest * nproc);
        for(int i = 0; i < nproc; i++, bytesInExcess--)
            if(bytesInExcess != 0) sizeBlocks[i]++; else break;
    }

    int currentFile = 0;
    int flag = 1;

    for(int i = 0; i < nproc; i++){
        dataDistTemp[i].size = 0;
        dataDistTemp[i].offset = 0;
    }

    for(int i = 0; i < nproc; i++){
        dataDistTemp[i].amount = sizeBlocks[i];
        sizeBlockLocal = sizeBlocks[i];
        flag = 1;

        if(i != 0){
            strcpy(&(dataDistTemp[i].fileNamesProcess[dataDistTemp[i].size * sizeFileName]),&fileNames[currentFile * sizeFileName]);
            printf("STO PASSANDO IL NOME DEL FILE: %s\n", &(dataDistTemp[i].fileNamesProcess[dataDistTemp[i].size * sizeFileName]));
            dataDistTemp[i].sizeFilesProcess[dataDistTemp[i].size] = sizeFiles[currentFile];
            printf("STO PASSANDO LA SIZE DEL FILE: %d\n", dataDistTemp[i].sizeFilesProcess[dataDistTemp[i].size]);
            dataDistTemp[i].size += 1;
            if(sizeBlockLocal >= (sizeFiles[currentFile] - dataDistTemp[i].offset)){
                sizeBlockLocal -= (sizeFiles[currentFile] - dataDistTemp[i].offset);
                currentFile++;
            } else {
                dataDistTemp[i+1].offset = dataDistTemp[i].offset + sizeBlockLocal;
                flag = 0;
            }
        }
        if(flag){
            for(int j = currentFile; j < files; j++){
                strcpy(&dataDistTemp[i].fileNamesProcess[dataDistTemp[i].size * sizeFileName],&fileNames[j * sizeFileName]);
                printf("STO PASSANDO IL NOME DEL FILE: %s\n", &(dataDistTemp[i].fileNamesProcess[dataDistTemp[i].size * sizeFileName]));
                dataDistTemp[i].sizeFilesProcess[dataDistTemp[i].size] = sizeFiles[currentFile];
                printf("STO PASSANDO LA SIZE DEL FILE: %d\n", dataDistTemp[i].sizeFilesProcess[dataDistTemp[i].size]);
                dataDistTemp[i].size += 1;
                if(sizeBlockLocal >= sizeFiles[j]){
                    sizeBlockLocal -= sizeFiles[j];
                } else {
                    dataDistTemp[i+1].offset = sizeBlockLocal;
                    currentFile = j;

                    break;
                }  
            }
        }
    }
}
int isChar(char c){
    int ch = (int) c;
    return (ch >= 65 && ch <= 90) || (ch >= 97 && ch <= 122);
}
void insertWord(HashTable* counts, char *temp, int countWord){
    size_t len = strlen(temp);
    char *lower = calloc(len+1, sizeof(char));

    for (size_t i = 0; i < len; ++i) {
        lower[i] = tolower((unsigned char) temp[i]);
    }

    void* value = ht_get(counts, lower);
    if (value != NULL) {
        // Already exists, increment int that value points to.
        int* pcount = (int*) value;
        (*pcount) += countWord;
    } else {
        // Word not found, allocate space for new int and set to countWord.
        int* pcount = malloc(sizeof(int));
        if (pcount == NULL) {
            exit_nomem();
        }
        *pcount = countWord;
        if (ht_set(counts, lower, pcount) == NULL) {
            exit_nomem();
        }
    }
}
MergedHashTable* merged_ht_create(int numEntries){
    // Allocate space for hash table struct.
    MergedHashTable* table = malloc(sizeof(MergedHashTable) * numEntries);
    if (table == NULL) {
        return NULL;
    }
    table->numEntries = numEntries;

    return table;
}
void merged_ht_destroy(MergedHashTable* table){
    free(table);
}
void merge(MergedHashTable* mergedTable, int l, int m, int r){
    int i, j, k;
    int n1 = m - l + 1;
    int n2 = r - m;

    // Vengono creati degli array temporanei
    int L1[n1], R1[n2];
    char L2[n1][WORD_LENGTH], R2[n2][WORD_LENGTH];
 
    // Vengono riempiti
    for (i = 0; i < n1; i++){
        strcpy(L2[i], mergedTable[l + i].word);
        L1[i] = mergedTable[l + i].frequencies;
    }
    for (j = 0; j < n2; j++){
        strcpy(R2[j], mergedTable[m + 1 + j].word);
        R1[j] = mergedTable[m + 1 + j].frequencies;
    }

    // Vengono uniti gli array temporanei negli array di partenza nella struttura dati
    i = 0; // Indice iniziale del primo sottoarray
    j = 0; // Indice iniziale del secondo sottoarray
    k = l; // Indice iniziale del sottoarray unito

    while (i < n1 && j < n2) {
        if (L1[i] > R1[j]) {
            strcpy(mergedTable[k].word, L2[i]);
            mergedTable[k].frequencies = L1[i];

            i++;
        }
        else if(L1[i] < R1[j]){
            strcpy(mergedTable[k].word, R2[j]);
            mergedTable[k].frequencies = R1[j];
            
            j++;
        } else {
            if(strcmp(L2[i], R2[j]) < 0){
                strcpy(mergedTable[k].word, L2[i]);
                mergedTable[k].frequencies = L1[i];

                i++;
            } else {
                strcpy(mergedTable[k].word, R2[j]);
                mergedTable[k].frequencies = R1[j];
                
                j++;
            }
        }
        k++;
    }
 
    // Vengono copiati gli elementi rimanenti del primo sottoarray, se ce ne sono
    while (i < n1) {
        strcpy(mergedTable[k].word, L2[i]);
        mergedTable[k].frequencies = L1[i];
        i++;
        k++;
    }
 
    // Vengono copiati gli elementi rimanenti del secondo sottoarray, se ce ne sono
    while (j < n2) {
        strcpy(mergedTable[k].word, R2[j]);
        mergedTable[k].frequencies = R1[j];
        j++;
        k++;
    }
}
void mergeSort(MergedHashTable* mergedTable, int l, int r){
    if (l < r) {
        // Uguale a (l+r)/2, ma evita l'overflow per l e h grandi
        int m = l + (r - l) / 2;
 
        // Ordiniamo ricorsivamente la prima e la seconda metà degli array
        mergeSort(mergedTable, l, m);
        mergeSort(mergedTable, m + 1, r);
 
        merge(mergedTable, l, m, r);
    }
}
