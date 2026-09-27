# How to pick the players on the scoreboard

This guide is for anyone setting up the scoreboard at the party. No technical knowledge is needed. All you need is a phone.

## Before you start

- The scoreboard must be plugged in and switched on.
- Your phone must be on the **same WiFi network** as the scoreboard. Ask whoever set it up which network that is. If the scoreboard runs on the host's phone hotspot, join that hotspot.
- Your phone must have internet access. The list of NFL players is downloaded from the internet.

## Step 1: Open the scoreboard's page

1. On your phone, open the web browser (Safari, Chrome or similar).
2. Type this into the address bar, exactly as written, and press Go:

   ```
   scoreboard.local
   ```

3. A page titled **Scoreboard** should appear.

### If that page does not open

Some phones cannot open `.local` addresses. Use the scoreboard's number address instead.

**If no players have been chosen yet**, the panel itself shows the address. It says **PICK PLAYERS**, and below that shows `http://` and two lines of numbers, for example:

```
http://
192.168.1
.42
```

Join the two number lines together and type the result into the browser: `192.168.1.42` in this example. Your numbers will be different.

**If players are already on the panel**, the address is not shown. Ask the person who runs the WiFi router to look up the device named **scoreboard** in the router's list of connected devices. The router shows its number address.

Still nothing? Check that your phone is on the right WiFi network, not on mobile data only.

## Step 2: Add players

1. Scroll down to **Add players**.
2. Wait until the search box says **Search by name or team**. The first time, this can take a few seconds while the player list downloads.
3. Type at least two letters of a player's name, for example `mahomes`.
4. Tap the player in the list. They move up to the **On the panel** list.
5. Repeat for each player. The panel holds **at most 9 players**.

To add a team defense, search for the team name, for example `green bay`, and pick the entry marked **DEF**.

## Step 3: Check the short names

The panel is small, so each player is shown with a short name. The page fills in the last name for you.

- To change a short name, tap the box next to the player and type a new one, for example `Patty` instead of `Mahomes`.
- If a name is too long, the panel leaves out letters from the middle, mostly vowels, so it stays readable. For example `Washington` becomes `Wshngtn`. For a different short form, type your own.
- **Name size** changes how many letters fit:

  | Name size | Letters | Players per screen |
  |---|---|---|
  | Large | about 6 | 4 |
  | Medium | about 7 or 8 | 5 |
  | Narrow | about 8 or 9 | 4 |

  Try one, tap **Save to panel**, and look at the panel.

## Step 4: Choose the scoring

Under the player list, set **Scoring** to match your league:

- **PPR**: one point per catch
- **Half PPR**: half a point per catch
- **Standard**: no points for catches

If you are not sure, ask the league commissioner. PPR is the most common.

## Step 5: Save

Tap the yellow **Save to panel** button.

- The message **Saved. Points refresh within a few seconds.** means it worked.
- The panel updates on its own within a few seconds.

**Nothing changes on the panel until you tap Save.** If the page says **Unsaved changes**, you still need to tap it.

## Changing things later

- **Remove a player:** tap the **✕** next to them, then tap **Save to panel**.
- **Swap a player:** remove one, add the other, then tap **Save to panel**.
- **Brightness** or **Name size:** change it, then tap **Save to panel**.
- The ↑ button changes the list order on the page. The panel itself always sorts players by points, highest first.

The scoreboard remembers the players even if it is unplugged.

## Reading the panel

- When it is switched on, the panel plays a short animation, ending with **GOOD AFTERNOON CHAMPIONS !!!**
- Each player gets a coloured stripe, the short name, then the points.
- Under the name, in tiny letters, is the score of that player's game:
  - `@MIA 1:00P`: the game has not started (`@` means an away game, `v` a home game)
  - `17-10 @MIA`: the game is on. The player's team is first, green when winning, red when losing. Every few seconds it shows the quarter and clock instead, for example `Q3 4:12`.
  - A small **yellow football** before the score: the player's team has the ball. It turns **red** near the end zone.
  - `35-14 @GB F`: final score
  - `BYE`: no game this week
- With more players than fit on one screen, the panel flips between pages every few seconds. Small dots at the bottom show which page is showing.
- The stripe colour shows the position:

  | Colour | Position |
  |---|---|
  | Red | QB |
  | Green | RB |
  | Blue | WR |
  | Orange | TE |
  | Purple | K |
  | Grey | DEF |

- The points and scores are for the current week. They update about every 30 seconds.
- A player's points turn **green** for a few seconds when they change.
- A grey **-** means the player has not played yet this week, or has a bye.
- If all the numbers turn grey, the scoreboard has lost its internet connection and the points may be out of date.

## Common problems

| What you see | What to do |
|---|---|
| The page never opens | Make sure your phone is on the same WiFi as the scoreboard. Try the number address (see Step 1). |
| The page says **Panel not reachable** | The phone lost contact with the scoreboard. Check the WiFi, then reload the page. |
| The search box says **Player list failed to load** | Your phone has no internet access. Connect to the internet and reload the page. |
| **The panel shows at most 9 players.** | Remove a player before adding another. |
| **Save failed** | Reload the page and try again. |
| A name on the panel is missing letters | That is on purpose: long names lose letters from the middle to fit. Type a shorter name yourself, or pick a different **Name size**. |
| A tiny red dot in the bottom-right corner | The scoreboard could not fetch the latest points or scores. It keeps trying every 30 seconds. |
| The panel says **WIFI..** | The scoreboard is still connecting to WiFi. Wait a minute. If it stays, the WiFi details in the scoreboard need fixing by whoever set it up. |
| The panel says **SET WIFI** | The scoreboard has no WiFi details. Whoever set it up needs to add them. |
