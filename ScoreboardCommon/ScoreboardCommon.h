/*
 * Common library for the scoreboard components
 */
#ifndef ScoreboardCommon_h
#define ScoreboardCommon_h

#define uint8_t byte
#define WIDE_NB_PER_OVER 2

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Const modes
const byte NO_MODE = 0;
const byte PLUS_MODE = 1;
const byte MINUS_MODE = 2;

// Const operating mode
const byte SCOREBOARD_OPMODE = 0;
const byte INNINGS_OPMODE = 1;
const byte TARGET_OPMODE = 2;
const byte RESET_OPMODE = 3;

// Radio pipe addresses for the 2 nodes to communicate over NRF24L01.
const uint64_t PIPES[2] = {0xF0F0F0F0E1LL, 0xF0F0F0F0D2LL};

struct score_t {
  int runs = 0;
  unsigned char overs = 0;
  unsigned char balls = 0;
  unsigned char extras = 0;
  unsigned char wickets = 0;
  unsigned char wideNBThisOver = 0;            // The number of wides/no balls this over
  int target = 0;
  unsigned char targetOvers = 0;
  unsigned char targetBalls = 0;

  bool operator==(const score_t& rhs) const {
    return (runs == rhs.runs &&
           overs == rhs.overs &&
           balls == rhs.balls &&
           extras == rhs.extras &&
           wickets == rhs.wickets &&
           wideNBThisOver == rhs.wideNBThisOver);
  }

  void run(byte mode) {
    if (mode == MINUS_MODE) {
      runs--;
      if (runs < 0) {
        runs = 0;
      }
    } else {
      runs++;                                  // Add one run
      if (mode != PLUS_MODE) {                 // If we're not in plus mode
        ball(mode);                            // Add one ball
      }
    }
  }

  /*
   * Records a ball being bowled
   */
  void ball(byte mode) {
    if (mode == MINUS_MODE) {
      if (balls == 0) {                          // If first ball of over
        overs--;                                 // Start new over
        balls = 5;                               // Reset to first ball
      } else {                                         // Otherwise
        balls--;                                 // Add one ball
      }
      if (overs < 0) {                           // Make sure overs haven't gone negative
        overs = 0;
        balls = 0;
      }
    } else {
      if (balls == 5) {                          // If last ball of over
        overs++;                                 // Start new over
        balls = 0;                               // Reset to first ball
        wideNBThisOver = 0;                      // Reset the number of wides and no balls
      } else {                                         // Otherwise
        balls++;                                 // Add one ball
      }
    }
  }

  void extra(byte mode, boolean modeFirstPress) {
    if (mode == MINUS_MODE) {
      extras--;
      runs--;
      if (modeFirstPress) wideNBThisOver--;
      if (extras < 0) extras = 0;
      if (runs < 0) runs = 0;
      if (wideNBThisOver < 0) wideNBThisOver = 0;
    } else {
      extras++;                                // Add one extra
      runs++;                                  // Add one run
      if (mode != PLUS_MODE) {
          wideNBThisOver++;                    // Add one to the number of wides/no balls this over
      }
      if (wideNBThisOver > WIDE_NB_PER_OVER) { // If we've exceed the number of wides/no balls allowed
        ball(mode);                            // then count this ball
      }
    }
  }

  /*
   * Records a leg bye
   */
  void legBye(byte mode) {
    if (mode == MINUS_MODE) {
      extras--;
      if (extras < 0) {
        extras = 0;
      }
    } else {
      extras++;                                // Add one extra
      run(mode);                               // Include the run
    }
  }

  /*
   * Records a wicket
   */
  void wicket(byte mode) {
    if (mode == MINUS_MODE) {
      wickets--;                                 // Subtract one wicket
      if (wickets < 0) {
        wickets = 0;
      }
    } else {
      if (wickets < 10) {
        wickets++;                               // Add one wicket
        if (mode != PLUS_MODE) {                 // If we're not in plus mode
          ball(mode);                            // Add one ball
        }
      }
    }
  }

  /*
   * Records a no score
   */
  void noScore(byte mode) {
    ball(mode);                                  // Add one ball
  }

  void targetRunsChange(byte mode) {
    if (mode == MINUS_MODE) {
      target--;
      if (target < 0) {
        target = 0;
      }
    } else {
      target++;
    }
  }

  void targetBallsChange(byte mode) {
    if (mode == MINUS_MODE) {
      if (targetBalls == 0) {                    // If first ball of over
        targetOvers--;                           // Start new over
        targetBalls = 5;                         // Reset to first ball
      } else {                                   // Otherwise
        targetBalls--;                           // Add one ball
      }
      if (targetOvers < 0) {                     // Make sure overs haven't gone negative
        targetOvers = 0;
        targetBalls = 0;
      }
    } else {
      if (targetBalls == 5) {                    // If last ball of over
        targetOvers++;                           // Start new over
        targetBalls = 0;                         // Reset to first ball
      } else {                                   // Otherwise
        targetBalls++;                           // Add one ball
      }
    }
  }

  float currentRunRate() {
    int ballsBowled = (overs * 6) + balls;
    if (ballsBowled == 0) {
      return 0.0;
    } else {
      return (((float)runs / ballsBowled) * 6.0);
    }
  }

  float targetRunRate() {
    // float i = ((score.runs / ((score.overs * 6.0) + score.balls)) * 6.0 * 10.0) + 0.5;
    int runsRemaining = target - runs;
    int ballsRemaining = (targetOvers * 6) + targetBalls - (overs * 6) - balls;
    if (ballsRemaining == 0) {
      return 0.0;
    } else {
      float r = (((float)runsRemaining / ballsRemaining) * 6.0);
      return r;
    }
  }
}; 

#endif  // ScoreboardCommon_h
