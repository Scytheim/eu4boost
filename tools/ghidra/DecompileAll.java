// Decompiles every function of the current program in parallel and writes the pseudo-C
// to sharded text files (eu4_decomp_000.c, ...) plus an index.
// Usage (headless, on an already analyzed project):
//   analyzeHeadless <projDir> <projName> -process eu4.exe -noanalysis -readOnly
//       -scriptPath <dir> -postScript DecompileAll.java <outDir> [functionsPerShard] [timeoutSeconds]
//@category EU4
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.DecompiledFunction;
import ghidra.app.decompiler.parallel.DecompileConfigurer;
import ghidra.app.decompiler.parallel.DecompilerCallback;
import ghidra.app.decompiler.parallel.ParallelDecompiler;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Program;
import ghidra.util.task.TaskMonitor;

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;

public class DecompileAll extends GhidraScript {

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		File outDir = new File(args.length > 0 ? args[0] : "decomp");
		int perShard = args.length > 1 ? Integer.parseInt(args[1]) : 2000;
		int timeout = args.length > 2 ? Integer.parseInt(args[2]) : 60;
		outDir.mkdirs();

		Program program = currentProgram;
		List<Function> all = new ArrayList<>();
		FunctionIterator it = program.getFunctionManager().getFunctions(true);
		while (it.hasNext()) {
			Function f = it.next();
			if (f.isExternal() || f.isThunk()) {
				continue;
			}
			all.add(f);
		}
		println("functions to decompile: " + all.size());

		DecompileConfigurer configurer = new DecompileConfigurer() {
			@Override
			public void configure(DecompInterface d) {
				DecompileOptions opts = new DecompileOptions();
				opts.grabFromProgram(program);
				d.setOptions(opts);
				d.toggleCCode(true);
				d.toggleSyntaxTree(false);
				d.setSimplificationStyle("decompile");
			}
		};

		AtomicInteger done = new AtomicInteger();
		AtomicInteger failed = new AtomicInteger();
		long start = System.currentTimeMillis();

		try (PrintWriter index = open(new File(outDir, "index.tsv"))) {
			index.println("entry\tname\tshard\tbody_bytes\tstatus");
			for (int shard = 0; shard * perShard < all.size() && !monitor.isCancelled(); shard++) {
				List<Function> chunk = all.subList(shard * perShard, Math.min(all.size(), (shard + 1) * perShard));
				Map<String, String> code = new ConcurrentHashMap<>();
				DecompilerCallback<Void> cb = new DecompilerCallback<Void>(program, configurer) {
					@Override
					public Void process(DecompileResults r, TaskMonitor m) throws Exception {
						Function f = r.getFunction();
						String key = f.getEntryPoint().toString();
						DecompiledFunction df = r.getDecompiledFunction();
						if (df != null && df.getC() != null) {
							code.put(key, df.getC());
						}
						else {
							code.put(key, "/* decompile failed: " + r.getErrorMessage() + " */\n");
							failed.incrementAndGet();
						}
						int n = done.incrementAndGet();
						if (n % 2000 == 0) {
							long s = (System.currentTimeMillis() - start) / 1000;
							println("decompiled " + n + " / " + all.size() + " (" + s + " s)");
						}
						return null;
					}
				};
				cb.setTimeout(timeout);
				ParallelDecompiler.decompileFunctions(cb, chunk, monitor);
				cb.dispose();

				String shardName = String.format("eu4_decomp_%03d.c", shard);
				try (PrintWriter w = open(new File(outDir, shardName))) {
					for (Function f : chunk) {
						String key = f.getEntryPoint().toString();
						String c = code.getOrDefault(key, "/* missing (timeout?) */\n");
						w.println("//==== " + key + " " + f.getName(true) + " size=" + f.getBody().getNumAddresses());
						w.println(c);
						index.println(key + "\t" + f.getName(true) + "\t" + shardName + "\t" + f.getBody().getNumAddresses() +
							"\t" + (code.containsKey(key) ? (c.startsWith("/* decompile failed") ? "fail" : "ok") : "missing"));
					}
				}
			}
		}
		long secs = (System.currentTimeMillis() - start) / 1000;
		try (PrintWriter s = open(new File(outDir, "summary.txt"))) {
			s.println("program\t" + program.getName());
			s.println("functions\t" + all.size());
			s.println("decompiled\t" + done.get());
			s.println("failed\t" + failed.get());
			s.println("seconds\t" + secs);
		}
		println("DecompileAll done: " + done.get() + " functions, " + failed.get() + " failed, " + secs + " s");
	}

	private static PrintWriter open(File f) throws Exception {
		return new PrintWriter(new OutputStreamWriter(new FileOutputStream(f), StandardCharsets.UTF_8));
	}
}
