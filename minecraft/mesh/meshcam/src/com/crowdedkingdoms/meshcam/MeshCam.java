package com.crowdedkingdoms.meshcam;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.ArrayDeque;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;

import com.mojang.blaze3d.pipeline.RenderTarget;

import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.minecraft.client.KeyMapping;
import net.minecraft.client.Minecraft;
import net.minecraft.client.Screenshot;
import net.minecraft.client.player.LocalPlayer;

/**
 * A camera for a load test's real game client: it writes the game's own frames to files and plays a short script, so
 * a crowd can be filmed with nobody at the keyboard and with the Mac's screen locked (when nothing outside the game
 * can capture its window or send it a key).
 *
 * <p>Everything is under {@code <game folder>/meshcam/}. {@code stats.csv} gets a line a second: wall-clock ms, the
 * game's frames a second, players and entities the client holds, and where the camera is. {@code control.txt} is read
 * whenever it changes; its lines run one after another:
 * <pre>
 * wait &lt;seconds&gt;
 * cmd &lt;command without the slash&gt;        chat &lt;text&gt;
 * shot &lt;name&gt;                             one frame, meshcam/&lt;name&gt;.png
 * rec &lt;name&gt; &lt;seconds&gt;                    a frame every game tick into meshcam/&lt;name&gt;/, while the next lines run
 * fly &lt;x&gt; &lt;y&gt; &lt;z&gt; &lt;yaw&gt; &lt;pitch&gt; &lt;seconds&gt;  moves there in a straight line (a spectator's or a flying player's camera)
 * turn &lt;yaw&gt; &lt;pitch&gt; &lt;seconds&gt;
 * walk &lt;yaw&gt; &lt;seconds&gt; [sprint] [jump]    holds forward, as a player would
 * hold &lt;attack|use|sneak|jump|tab&gt; &lt;seconds&gt;  holds that key (attack and use act on the block under the crosshair; tab shows the player list)
 * f3 &lt;on|off&gt;                              the game's debug overlay, which shows its frames a second
 * </pre>
 * Frames are numbered from 00000; ffmpeg turns a folder into a movie at 20 frames a second, the game's tick rate.
 * The movie's smoothness is the tick rate's, not the frame rate's: that is in stats.csv.
 */
public final class MeshCam implements ClientModInitializer {
	private record Step(String[] words, String line) {
	}

	private final ArrayDeque<Step> steps = new ArrayDeque<>();
	private final ExecutorService writers = Executors.newFixedThreadPool(4, runnable -> {
		Thread thread = new Thread(runnable, "meshcam-writer");
		thread.setDaemon(true);
		return thread;
	});
	private final AtomicInteger writing = new AtomicInteger();
	private Path folder;
	private long controlStamp = -1;
	private int tick;
	// The step under way.
	private Step step;
	private int stepTicks;
	private int stepLength;
	private double fromX, fromY, fromZ, toX, toY, toZ;
	private float fromYaw, fromPitch, toYaw, toPitch;
	private KeyMapping held;
	// The recording under way.
	private Path recording;
	private int recordTicksLeft;
	private int frame;

	@Override
	public void onInitializeClient() {
		ClientTickEvents.END_CLIENT_TICK.register(this::tick);
	}

	private void tick(Minecraft minecraft) {
		LocalPlayer player = minecraft.player;
		if (player == null || minecraft.level == null) {
			return;
		}
		if (folder == null) {
			folder = minecraft.gameDirectory.toPath().resolve("meshcam");
			try {
				Files.createDirectories(folder);
			} catch (IOException e) {
				return;
			}
			log("in the world");
		}
		tick++;
		// A minute without input clamps the game to 30 frames a second: the camera counts as someone playing.
		minecraft.getFramerateLimitTracker().onInputReceived();
		if (tick % 20 == 0) {
			stats(minecraft, player);
			readControl();
		}
		if (recording != null) {
			grab(minecraft, recording.resolve(String.format("%05d.png", frame++)));
			if (--recordTicksLeft <= 0) {
				log("recorded " + frame + " frames into " + recording.getFileName());
				recording = null;
			}
		}
		if (step == null && !steps.isEmpty()) {
			begin(minecraft, player, steps.poll());
		}
		if (step != null) {
			advance(minecraft, player);
		}
	}

	private void stats(Minecraft minecraft, LocalPlayer player) {
		String line = System.currentTimeMillis() + "," + minecraft.getFps() + "," + minecraft.level.players().size() + "," + minecraft.level.getEntityCount()
				+ "," + String.format("%.1f,%.1f,%.1f", player.getX(), player.getY(), player.getZ()) + "," + writing.get() + "\n";
		try {
			Files.writeString(folder.resolve("stats.csv"), line, StandardCharsets.UTF_8, StandardOpenOption.CREATE, StandardOpenOption.APPEND);
		} catch (IOException e) {
			// The next second's line may go.
		}
	}

	private void readControl() {
		File control = folder.resolve("control.txt").toFile();
		long stamp = control.lastModified();
		if (stamp == 0 || stamp == controlStamp) {
			return;
		}
		controlStamp = stamp;
		try {
			List<String> lines = Files.readAllLines(control.toPath(), StandardCharsets.UTF_8);
			steps.clear();
			for (String line : lines) {
				String trimmed = line.trim();
				if (!trimmed.isEmpty() && !trimmed.startsWith("#")) {
					steps.add(new Step(trimmed.split("\\s+"), trimmed));
				}
			}
			log("script of " + steps.size() + " steps");
		} catch (IOException e) {
			log("could not read control.txt: " + e);
		}
	}

	private void begin(Minecraft minecraft, LocalPlayer player, Step next) {
		String[] w = next.words();
		step = next;
		stepTicks = 0;
		stepLength = 1;
		held = null;
		try {
			switch (w[0]) {
				case "wait" -> stepLength = ticks(w[1]);
				case "cmd" -> player.connection.sendCommand(next.line().substring(4).trim());
				case "chat" -> player.connection.sendChat(next.line().substring(5).trim());
				case "shot" -> grab(minecraft, folder.resolve(w[1] + ".png"));
				case "rec" -> {
					recording = folder.resolve(w[1]);
					Files.createDirectories(recording);
					recordTicksLeft = ticks(w[2]);
					frame = 0;
				}
				case "fly", "turn" -> {
					boolean fly = w[0].equals("fly");
					fromX = player.getX();
					fromY = player.getY();
					fromZ = player.getZ();
					fromYaw = player.getYRot();
					fromPitch = player.getXRot();
					toX = fly ? Double.parseDouble(w[1]) : fromX;
					toY = fly ? Double.parseDouble(w[2]) : fromY;
					toZ = fly ? Double.parseDouble(w[3]) : fromZ;
					toYaw = Float.parseFloat(w[fly ? 4 : 1]);
					toPitch = Float.parseFloat(w[fly ? 5 : 2]);
					// The short way round.
					float turn = ((toYaw - fromYaw) % 360 + 540) % 360 - 180;
					toYaw = fromYaw + turn;
					stepLength = ticks(w[fly ? 6 : 3]);
				}
				case "walk" -> {
					player.setYRot(Float.parseFloat(w[1]));
					stepLength = ticks(w[2]);
					held = minecraft.options.keyUp;
					for (int i = 3; i < w.length; i++) {
						if (w[i].equals("sprint")) {
							minecraft.options.keySprint.setDown(true);
						} else if (w[i].equals("jump")) {
							minecraft.options.keyJump.setDown(true);
						}
					}
				}
				case "hold" -> {
					held = switch (w[1]) {
						case "attack" -> minecraft.options.keyAttack;
						case "use" -> minecraft.options.keyUse;
						case "sneak" -> minecraft.options.keyShift;
						case "tab" -> minecraft.options.keyPlayerList;
						default -> minecraft.options.keyJump;
					};
					stepLength = ticks(w[2]);
				}
				case "f3" -> minecraft.debugEntries.setOverlayVisible(w[1].equals("on"));
				default -> log("unknown step: " + next.line());
			}
		} catch (RuntimeException | IOException e) {
			log("step failed: " + next.line() + " (" + e + ")");
			stepLength = 1;
		}
		log("step: " + next.line());
	}

	private void advance(Minecraft minecraft, LocalPlayer player) {
		stepTicks++;
		String kind = step.words()[0];
		if (kind.equals("fly") || kind.equals("turn")) {
			double t = Math.min(1.0, stepTicks / (double) stepLength);
			if (kind.equals("fly")) {
				player.setPos(fromX + (toX - fromX) * t, fromY + (toY - fromY) * t, fromZ + (toZ - fromZ) * t);
				player.setDeltaMovement(0, 0, 0);
			}
			player.setYRot((float) (fromYaw + (toYaw - fromYaw) * t));
			player.setXRot((float) (fromPitch + (toPitch - fromPitch) * t));
		}
		if (held != null) {
			held.setDown(true);
		}
		if (stepTicks >= stepLength) {
			if (held != null) {
				held.setDown(false);
				minecraft.options.keySprint.setDown(false);
				minecraft.options.keyJump.setDown(false);
				held = null;
			}
			step = null;
		}
	}

	private void grab(Minecraft minecraft, Path file) {
		RenderTarget target = minecraft.gameRenderer.mainRenderTarget();
		// A Retina window's frame is four times the pixels: halved, it is the window's size in points.
		int downscale = target.width > 1600 ? 2 : 1;
		writing.incrementAndGet();
		Screenshot.takeScreenshot(target, downscale, image -> writers.execute(() -> {
			try (image) {
				image.writeToFile(file);
			} catch (IOException e) {
				log("could not write " + file.getFileName() + ": " + e);
			} finally {
				writing.decrementAndGet();
			}
		}));
	}

	private static int ticks(String seconds) {
		return Math.max(1, (int) Math.round(Double.parseDouble(seconds) * 20));
	}

	private void log(String message) {
		System.out.println("[meshcam] " + message);
		try {
			Files.writeString(folder.resolve("log.txt"), System.currentTimeMillis() + " " + message + "\n", StandardCharsets.UTF_8, StandardOpenOption.CREATE,
					StandardOpenOption.APPEND);
		} catch (IOException | RuntimeException e) {
			// Only the log.
		}
	}
}
