
#include <limits.h>

#if 0
#define DEBUG_UMP3(x) Serial.println((x))
#else
#define DEBUG_UMP3(x) ;
#endif

TrackManager::TrackManager()
{
  track_index_sz = 0;
}

bool TrackManager::AddTrack(char *str)
{
  int year;
  int suffix; // actually, we dont use this
  
  sscanf(str, "%de%d.mp3", &year, &suffix);
  
  for (int i=0; i<track_index_sz; i++) {
    if (years[i] == year) {
      num_tracks[i] += 1;
      return true;
    }
  }
  if (track_index_sz == TRACK_INDEX_MAX_SZ-1)
    return false;
  years[track_index_sz] = year;
  num_tracks[track_index_sz] = 1;
  track_index_sz++;
  return true;
}

bool TrackManager::GetRandomTrack(char *file_name, int year)
{
  for (int i=0; i<track_index_sz; i++) {
    if (years[i] == year) {
      // found the year
      int suffix = random(num_tracks[i]);
      sprintf(file_name, "%de%02d.mp3", year, suffix);
      return true;
    }
  }
  return false;  
}

int TrackManager::GetRandomYear()
{
  int i = random(track_index_sz);
  return years[i]; 
}

RogueMP3 ump3(ump3_serial);

#define LINE_BUF_SZ 64

//
//  tracks_init
//
//  Initialize the interface to the mp3 board.
//  Also populates the track table.
//
int tracks_init(void)
{
  int idx;
  long baud;
  char buf[LINE_BUF_SZ];
  char file_name[FILE_NAME_MAX_SZ];
  
  DEBUG_UMP3("tracks_init");

  idx = ump3.sync();
  DEBUG_UMP3(String("ump3.sync() ") + String(idx));
  ump3.stop();

  int num_tracks = 0;

  idx = 0;
  ump3_serial.print("FC L /\r");
  
  while(ump3_serial.peek() != '>')
  {
    idx = 0;

    // read the whole line
    do
    {
      while(!ump3_serial.available());
      buf[idx++] = ump3_serial.read();
    } while(buf[idx-1] != 0x0D);
    
    // replace the trailing CR with a null
    buf[idx-1] = 0;
    
    DEBUG_UMP3(String("uMP3 rx: ") + String(buf));
    //Serial.println(buf);

    if( 1 == sscanf(buf, "%*d %s", &file_name))
    {
      tm.AddTrack(file_name);
      num_tracks++;
    }

    // wait here until next character is available
    while(!ump3_serial.available());
  }
  ump3_serial.read(); // read the '>'

  Serial.print(num_tracks);
  Serial.println(" tracks on the card");
  
  return 0;
}

void play_track(char *file_name)
{
   ump3.sync();
   
   ump3_serial.print("PC F /");
   ump3_serial.print(file_name);
   ump3_serial.print("\n");
   
   ump3.stop();

}

char get_playback_status()
{
  return ump3.getplaybackstatus();
}

  
  
