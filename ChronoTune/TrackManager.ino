
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



  
  
