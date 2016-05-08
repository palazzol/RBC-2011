#ifndef _TRACKS_H_
#define _TRACKS_H_

#define TRACK_INDEX_MAX_SZ 128
#define FILE_NAME_MAX_SZ 16

#define ump3_serial Serial1 

extern RogueMP3 ump3;

class TrackManager
{
  public:
    // Constructor
    TrackManager();
    // Populate the index with this
    bool AddTrack(char *fname);
    // Retrieve filenames to play with this
    bool GetRandomTrack(char *file_name, int year); 

  private:
    int years[TRACK_INDEX_MAX_SZ];
    int num_tracks[TRACK_INDEX_MAX_SZ];
    int track_index_sz;
};

#endif

