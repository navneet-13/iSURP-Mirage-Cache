// Copyright (C) 2021, Gururaj Saileshwar, Moinuddin Qureshi: Georgia Institute of Technology.

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <tuple>
#include <vector>
#include <unordered_map>
#include "mtrand.h"

using namespace std;

/////////////////////////////////////////////////////
// COMMAND-LINE ARGUMENTS
/////////////////////////////////////////////////////
//argv[1] : EXTRA TAGS PER SET (PER SKEW)
int EXTRA_BUCKET_CAPACITY = 2;

//argv[2] : NUMBER OF BALLS THROWN
int NUM_BILLION_TRIES = 1;

//argv[3] : SEED
unsigned int myseed = 1;

/////////////////////////////////////////////////////
// DEFINES
/////////////////////////////////////////////////////
//Cache Configuration
//Default: 16 Way LLC
#ifndef CUSTOM_BASE_WAYS_PER_SKEW
#define BASE_WAYS_PER_SKEW        (8)
#else
#define BASE_WAYS_PER_SKEW        (CUSTOM_BASE_WAYS_PER_SKEW)
#endif
#define NUM_SKEWS                 (2)

//16MB LLC TAG ARRAY
#define CACHE_SZ_BYTES            (16*1024*1024) 
#define LINE_SZ_BYTES             (64)
#define NUM_BUCKETS               ((CACHE_SZ_BYTES/LINE_SZ_BYTES)/(BASE_WAYS_PER_SKEW))
#define NUM_BUCKETS_PER_SKEW      (NUM_BUCKETS/NUM_SKEWS)

//8MB LLC DATA ARRAY
#ifndef CUSTOM_DATA_SZ
#define DATA_SZ                   (262144/2)
#else
#define DATA_SZ                   (CUSTOM_DATA_SZ)
#endif //

//Bucket Capacities
#define BALLS_PER_BUCKET      (BASE_WAYS_PER_SKEW)
#ifndef CUSTOM_MAX_FILL
#define MAX_FILL              (32)
#else
#define MAX_FILL              (CUSTOM_MAX_FILL)
#endif
int SPILL_THRESHOLD = BALLS_PER_BUCKET + EXTRA_BUCKET_CAPACITY;

//NAV: Number of ball ids that will be used to randomly generate a ball to be thrown
#ifndef NUM_BALL_ID
#define TOTAL_BALL_ID         (4*262144)
#else
#define TOTAL_BALL_ID         (NUM_BALL_ID)
#endif

// Tie-Breaking Policy between Skews on Ball-Throws
//0 - Randomly picks either skew on Ties. 
//1 - Always picks Skew-0 on Ties.
#define BREAK_TIES_PREFERENTIALLY      (0)

//Experiment Size
#define BILLION_TRIES             (1000*1000*1000)
#define HUNDRED_MILLION_TRIES     (100*1000*1000)

//Types
typedef unsigned int uns;
typedef unsigned long long uns64;
typedef double dbl;


/////////////////////////////////////////////////////
// EXPERIMENT VARIABLES
/////////////////////////////////////////////////////

// NAV: Create a hash table to store the set and priority of ball_ids
unordered_map<uns64, tuple<uns64, uns64>> set_map;

//For each Bucket (Set), number of Valid Balls in Bucket
uns64 bucket[NUM_BUCKETS];

//For each Ball (Cache-Line), which Bucket (Set) it is in
//(Data-Structure Similar to Data-Store RPTR)
//uns64 balls[NUM_BUCKETS*BALLS_PER_BUCKET];

//Number of Times Each Bucket Indexed 
uns64 bucket_fill_observed[MAX_FILL+1];

//Number of Times Bucket Containing N Balls has a Ball-Insertion
uns64 stat_counts[MAX_FILL+1];

//Number of Spills from Buckets
uns64 spill_count = 0;

//Number of Spills despite relocation attempts.
uns64 cuckoo_spill_count = 0;

//Tracks if Initialization of Buckets Done
bool init_buckets_done = false;

//Mersenne Twister Rand Generator
MTRand *mtrand=new MTRand();

/////////////////////////////////////////////////////
// FUNCTIONS - Spill, Ball Insertion, Removal etc.
/////////////////////////////////////////////////////

/////////////////////////////////////////////////////
// Spill Ball: relocating filled bucket
// -- Based on which skew spill happened;
// -- cuckoo into other recursively.
/////////////////////////////////////////////////////

void spill_ball(uns64 index, uns64 ballID){
  uns done=0;

  bucket[index]--;

  while(done!=1){
    //Pick skew & bucket-index where spilled ball should be placed.
    uns64 spill_index ;
    //If current index is in Skew0, then pick Skew1. Else vice-versa.
    if(index < NUM_BUCKETS_PER_SKEW)
      spill_index = NUM_BUCKETS_PER_SKEW + mtrand->randInt(NUM_BUCKETS_PER_SKEW-1);
    else
      spill_index = mtrand->randInt(NUM_BUCKETS_PER_SKEW-1);

    vector<int>realloc_list;
    for (auto i = set_map.begin(); i != set_map.end(); i++) {
      if (get<0>(i->second) == index && (i->first) != ballID) {
        realloc_list.push_back(i->first);
      }
    }
    uns64 randomID = realloc_list[mtrand->randInt(realloc_list.size() - 1)];
    uns64 priority = get<1>(set_map[randomID]);  //.at changed
    set_map[randomID] = make_tuple(spill_index, priority);

    //If new spill_index bucket where spilled-ball is to be installed has space, then done.
    if(bucket[spill_index] < SPILL_THRESHOLD){
      done=1;
      bucket[spill_index]++;
      //balls[ballID] = spill_index;
      
    } 
    else {
      assert(bucket[spill_index] == SPILL_THRESHOLD);
      //if bucket of spill_index is also full, then recursive-spill, we call this a cuckoo-spill
      index = spill_index;
      ballID = randomID;
      cuckoo_spill_count++;
    }
  }

  spill_count++;
}

/////////////////////////////////////////////////////
// Insert Ball in Bucket
/////////////////////////////////////////////////////
uns64 insert_ball(uns64 ballID){
    //Index for Rand Bucket in Skew-0
  uns64 index1 = mtrand->randInt(NUM_BUCKETS_PER_SKEW - 1);
  //Index for Rand Bucket in Skew-1
  uns64 index2 = NUM_BUCKETS_PER_SKEW + mtrand->randInt(NUM_BUCKETS_PER_SKEW - 1);

  //Increments Tracking of Indexed Buckets
  if(init_buckets_done){
    bucket_fill_observed[bucket[index1]]++;
    bucket_fill_observed[bucket[index2]]++;
  }
    
  uns64 index;
  uns64 retval;

  //------ LOAD AWARE SKEW SELECTION -----
  //Identify which Bucket (index) has Less Load
  if(bucket[index2] < bucket[index1]){
    index = index2;
  } else if (bucket[index1] < bucket[index2]){
    index = index1;    
  } else if (bucket[index2] == bucket[index1]) {

#if BREAK_TIES_PREFERENTIALLY == 0
    //Break ties randomly
    if(mtrand->randInt(1) == 0)
      index = index1;
    else
      index = index2;

#elif BREAK_TIES_PREFERENTIALLY == 1
    //Break ties favoring Skew-0.
    index = index1;
#endif
     
  } else {
    assert(0);
  }

  //Increments count for Bucket where Ball Inserted 
  retval = bucket[index];
  bucket[index]++;

  //Track which bucket the new Ball was inserted in
  //assert(balls[ballID] == (uns64)-1);
  //balls[ballID] = index;

  // AB: add the new ball to the hash function
  set_map[ballID] = make_tuple(index, 0);
  
  //----------- SPILL --------
  if(SPILL_THRESHOLD && (retval >= SPILL_THRESHOLD)){
    //Overwrite balls[ballID] with spill_index.
    spill_ball(index,ballID);   
  }

  // Return num-balls in bucket where new ball inserted.
  return retval;

}

/////////////////////////////////////////////////////
// Remove Random Ball (Global Eviction)
/////////////////////////////////////////////////////
void remove_ball(){
  // AB: creating a vector containing ballIDs stored in the Data Array
  vector<uns64>keys_list;
  for (auto i = set_map.begin(); i != set_map.end(); i++) {
    if (get<1>(i->second) == 1) {
      keys_list.push_back(i->first);
    }
  }

  // AB: random eviction from data array
  uns64 randomID = keys_list[mtrand->randInt(keys_list.size() - 1)];
  
  // AB: decrementing valid entries for set corresponding to randomID
  assert(bucket[get<0>(set_map[randomID])] != 0 ); 
  bucket[get<0>(set_map[randomID])]--;
  
  // AB: removing the randomID entry from the data array
  set_map.erase(randomID);
}

void tag_hit(uns64 ballID){
  assert(set_map.find(ballID) != set_map.end());
  uns64 set;
  uns64 priority;
  set = get<0>(set_map[ballID]);
  priority = get<1>(set_map[ballID]);

  assert(priority != -1);

  if (priority == 0){
    // AB: changing the priority for the entry to 1
    set_map[ballID] = make_tuple(set, 1);
    //printf("Remove ball called in tag hit \n");
    remove_ball();
  }  
  else {
      // AB: might need to increment access counter
  }
}

uns64 tag_miss(uns64 ballID){
  assert(set_map.find(ballID) == set_map.end());
  uns64 retval = insert_ball(ballID);
  //printf("Remove ball called \n");
  remove_ball();
  return retval;
}

void throw_ball(){
  uns64 randID = mtrand->randInt(TOTAL_BALL_ID - 1);
  //printf("\nBall ID: %d \n", randID);

  if (set_map.find(randID) != set_map.end()){
    //printf("Tag hit occured \n");
    tag_hit(randID);
  }
  else{
    //printf("Tag miss occured \n");
    uns64 res = tag_miss(randID);
    if(res <= MAX_FILL){
      stat_counts[res]++;
    }else{
      printf("Overflow\n");
      exit(-1);
    }
  }

  //printf("Res: %u\n", res);
  //return res;
}

// uns64 remove_ball(void){
//   // Select Random BallID from all Balls
//   uns64 ballID = mtrand->randInt(NUM_BUCKETS*BALLS_PER_BUCKET -1);

//   // Identify which bucket this ball is in 
//   assert(balls[ballID] != (uns64)-1);
//   uns64 bucket_index = balls[ballID];

//   // Update Ball Tracking
//   assert(bucket[bucket_index] != 0 );  
//   bucket[bucket_index]--;
//   balls[ballID] = -1;

//   // Return BallID removed (ID will be reused for new ball to be inserted)  
//   return ballID;
// }

/////////////////////////////////////////////////////
/////////////////////////////////////////////////////

void display_histogram(){
  uns ii;
  uns s_count[MAX_FILL+1];

  for(ii=0; ii<= MAX_FILL; ii++){
    s_count[ii]=0;
  }

  for(ii=0; ii< NUM_BUCKETS; ii++){
    s_count[bucket[ii]]++;
  }

  //  printf("\n");
  // AB: keeps a track of percentage of buckets having ii number of balls
  printf("\nOccupancy: \t\t Count");
  for(ii=0; ii<= MAX_FILL; ii++){
    double perc = 100.0 * (double)s_count[ii]/(double)(NUM_BUCKETS);
    printf("\nBucket[%2u Fill]: \t %u \t (%4.2f)", ii, s_count[ii], perc);
  }

  printf("\n");
}

/////////////////////////////////////////////////////
/////////////////////////////////////////////////////

void sanity_check(void){
  uns count=0;
  // uns s_count[MAX_FILL+1];

  // for(ii=0; ii<= MAX_FILL; ii++){
  //   s_count[ii]=0;
  // }
  
  // for(uns ii=0; ii< NUM_BUCKETS; ii++){
  //   count += bucket[ii];
  //   //s_count[bucket[ii]]++;
  // }
  
  for (auto i = set_map.begin(); i != set_map.end(); i++) {
    if (get<1>(i->second) == 1) {
      count++;
    }
  }

  // AB: number of entries in the Data Array exceeds the capacity
  if(count != (DATA_SZ)){
    printf("\n*** Sanity Check Failed, TotalCount : %u*****\n", count);
  }
}

/////////////////////////////////////////////////////
// Randomly Initialize all the Buckets with Balls (NumBuckets * BallsPerBucket) 
/////////////////////////////////////////////////////
//NAV: Changing init buckets to init_setmap to fill the data array (init_buckets -> init)
void init(void){
  uns64 ii;

  assert(NUM_SKEWS * NUM_BUCKETS_PER_SKEW == NUM_BUCKETS);
  
  // AB: initialize all buckets to have all invalid tags
  for(ii=0; ii<NUM_BUCKETS; ii++){
    bucket[ii]=0;
  }
 
  for(ii=0; ii<(TOTAL_BALL_ID); ii+= uns64((TOTAL_BALL_ID/DATA_SZ))){
    //balls[ii] = -1;
    // AB: insert incoming ball into the set_map without evicting random line
    uns64 ball1 = insert_ball(ii);           
    // AB: modify the priority of this ball to 1 (with data)
    uns64 set = get<0>(set_map[ii]);
    set_map[ii] = make_tuple(set, 1);
    // AB: insert a different ball into the tag store with priority 0
    uns64 ball2 = insert_ball(ii+4);
  }

  for(ii=0; ii<=MAX_FILL; ii++){
   stat_counts[ii]=0;
  }

  sanity_check();
  init_buckets_done = true;
  printf("Init Done\n");
}

/////////////////////////////////////////////////////
// Randomly remove a ball and
// then insert a new ball with Power-of-Choices Install
/////////////////////////////////////////////////////

// uns  remove_and_insert(void){
  
//   uns res = 0;

//   uns64 ballID = remove_ball();
//   res = insert_ball(ballID);

//   if(res <= MAX_FILL){
//     stat_counts[res]++;
//   }else{
//     printf("Overflow\n");
//     exit(-1);
//   }

//   //printf("Res: %u\n", res);
//   return res;
// }



/////////////////////////////////////////////////////
/////////////////////////////////////////////////////

int main(int argc, char* argv[]){

  //Get arguments:
  assert((argc == 4) && "Need 3 arguments: (EXTRA_BUCKET_CAPACITY:[0-8] BN_BALL_THROWS:[1-10^5] SEED:[1-400])");
  EXTRA_BUCKET_CAPACITY = atoi(argv[1]);
  SPILL_THRESHOLD = BASE_WAYS_PER_SKEW + EXTRA_BUCKET_CAPACITY;
  NUM_BILLION_TRIES  = atoi(argv[2]);
  myseed = atoi(argv[3]);

  printf("Cache Configuration: %d MB, %d skews, %d ways (%d ways/skew)\n",CACHE_SZ_BYTES/1024/1024,NUM_SKEWS,NUM_SKEWS*BASE_WAYS_PER_SKEW,BASE_WAYS_PER_SKEW);
  printf("AVG-BALLS-PER-BUCKET:%d, BUCKET-SPILL-THRESHOLD:%d \n",BASE_WAYS_PER_SKEW,SPILL_THRESHOLD);
  printf("Simulation Parameters - BALL_THROWS:%llu, SEED:%d\n\n",(unsigned long long)NUM_BILLION_TRIES*(unsigned long long)BILLION_TRIES,myseed);
  
  uns64 ii;
  mtrand->seed(myseed);

  //Initialize Buckets
  printf("Initializing..... \n");
  init();

  //Ensure Total Balls in Buckets is Conserved.
  sanity_check(); // AB: redundant
  printf("Sanity Check Successful \n");

  printf("Starting --  (Dot printed every 100M Ball throws) \n");

  //N Billion Ball Throws
  /*
  for (uns64 bn_i=0 ; bn_i < NUM_BILLION_TRIES; bn_i++) {    
    //1 Billion Ball Throws
    for(uns64 hundred_mn_count=0; hundred_mn_count<10; hundred_mn_count++){
      //In multiples of 100 Million Ball Throws.
      for(ii=0; ii<HUNDRED_MILLION_TRIES; ii++){
        //Insert and Remove Ball
        throw_ball();
        //printf("%d ", ii);

      }
      printf(".");fflush(stdout);
    }    
    //Ensure Total Balls in Buckets is Conserved.
    sanity_check();
    //Print count of Balls Thrown.
    printf(" %dBn\n",bn_i+1);fflush(stdout);    
  }
  */
  
  for (uns64 i = 0; i < 1000; i++){
    for (uns64 ii = 0; ii < 1000; ii++) {
      throw_ball();
      printf("%lld%lld \n", i, ii);
    }
  }

  printf("\n\nBucket-Occupancy Snapshot at End of Experiment\n");
  display_histogram();
  printf("\n\n");

  printf("Distribution of Bucket-Occupancy (Averaged across Ball Throws) => Used for P(Bucket = k balls) calculation \n");
  printf("\nOccupancy: \t\t %16s \t P(Bucket=k balls)","Count");
  for(ii=0; ii<= MAX_FILL; ii++){
    double perc = 100.0 * (double)bucket_fill_observed[ii]/(NUM_SKEWS*(double)NUM_BILLION_TRIES*(double)BILLION_TRIES);
    printf("\nBucket[%2llu Fill]: \t %16llu \t (%5.3f)", ii, bucket_fill_observed[ii], perc);
  }

  printf("\n\n\n");

  printf("Distribution of Balls-in-Dest-Bucket on Ball-Insertion (Best-Of-2 Indexed-Buckets) => Spill-Count = Spills from Bucket-With-%d-Balls\n",SPILL_THRESHOLD);
  //  printf("\n");
  printf("\nBalls-in-Dest-Bucket (k) \t\t Spills from Bucket-With-k-Balls)\n");
  for(ii=0; ii<MAX_FILL; ii++){
    double perc = 100.0*(double)(stat_counts[ii])/(double)((double)NUM_BILLION_TRIES*(double)BILLION_TRIES);
    printf("%2llu:\t\t\t\t %16llu\t (%5.3f)\n", ii, stat_counts[ii], perc);
  }

  printf("\nSpill Count: %llu (%5.3f)\n", spill_count,
         100.0* (double)spill_count/(double)((double)NUM_BILLION_TRIES*(double)BILLION_TRIES));
  printf("\nCuckoo Spill Count: %llu (%5.3f)\n", cuckoo_spill_count,
         100.0* (double)cuckoo_spill_count/(double)((double)NUM_BILLION_TRIES*(double)BILLION_TRIES));

  return 0;
}


