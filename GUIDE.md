# How to pick the players on the scoreboard

This guide is for anyone setting up the scoreboard at the party. No technical knowledge is needed. All you need is a phone.

## Before you start

- The scoreboard must be plugged in and switched on.
- Your phone must be on the **same WiFi network** as the scoreboard. Ask whoever set it up which network that is.
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
5. Repeat for each player. The panel holds **at most 8 players**.

To add a team defense, search for the team name, for example `green bay`, and pick the entry marked **DEF**.

## Step 3: Check the short names

The panel is small, so each player is shown with a short name of up to **6 letters**. The page fills in the last name for you.

- To change a short name, tap the box next to the player and type a new one, for example `Patty` instead of `Mahome`.
- Letters after the sixth are cut off on the panel.

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
- **Brightness:** drag the **Brightness** slider, then tap **Save to panel**.
- The ↑ button changes the list order on the page. The panel itself always sorts players by points, highest first.

The scoreboard remembers the players even if it is unplugged.

## Reading the panel

- Each row is one player: a coloured stripe, the short name, then the points.
- The stripe colour shows the position:

  | Colour | Position |
  |---|---|
  | Red | QB |
  | Green | RB |
  | Blue | WR |
  | Orange | TE |
  | Purple | K |
  | Grey | DEF |

- The points are for the current week. They update about once a minute.
- A row turns **green** for a few seconds when that player's points change.
- A grey **-** means the player has not played yet this week, or has a bye.
- If all the numbers turn grey, the scoreboard has lost its internet connection and the points may be out of date.

## Common problems

| What you see | What to do |
|---|---|
| The page never opens | Make sure your phone is on the same WiFi as the scoreboard. Try the number address (see Step 1). |
| The page says **Panel not reachable** | The phone lost contact with the scoreboard. Check the WiFi, then reload the page. |
| The search box says **Player list failed to load** | Your phone has no internet access. Connect to the internet and reload the page. |
| **The panel shows at most 8 players.** | Remove a player before adding another. |
| **Save failed** | Reload the page and try again. |
| The panel says **WIFI..** | The scoreboard is still connecting to WiFi. Wait a minute. If it stays, the WiFi details in the scoreboard need fixing by whoever set it up. |
| The panel says **SET WIFI** | The scoreboard has no WiFi details. Whoever set it up needs to add them. |
