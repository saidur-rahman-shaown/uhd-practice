function analyze_capture(cap_path, zc_length, zc_root)
%ANALYZE_CAPTURE  Correlate an rx_capture file against a Zadoff-Chu sequence.
%
%   analyze_capture                                     % all defaults
%   analyze_capture('~/captures/cap.fc32')
%   analyze_capture('~/captures/cap.fc32', 401, 25)     % length, root
%
% The sequence is generated here rather than read from disk, so this needs
% only the capture -- nothing has to be copied from the transmitting machine.
% Pass the length and root the transmitter used; they default to the values in
% zc_transmit.
%
% The capture is raw interleaved complex float32, which is what UHD produces
% for the "fc32" host format. Its sidecar .txt carries the sample rate.
%
% What to look for: the peak spacing should equal the sequence length. A tall
% peak at an arbitrary offset is the stream-startup transient, which
% correlates against anything; a peak every N samples is the sequence.

    if nargin < 1 || isempty(cap_path),  cap_path  = '~/captures/cap.fc32'; end
    if nargin < 2 || isempty(zc_length), zc_length = 401;                   end
    if nargin < 3 || isempty(zc_root),   zc_root   = 25;                    end

    meta = read_sidecar(cap_path);
    fs   = getfield_default(meta, 'rate_hz', 30.72e6);

    x  = read_fc32(cap_path);
    zc = zadoff_chu(zc_length, zc_root);
    N  = numel(zc);

    fprintf('  capture   : %d samples, %.1f ms at %.3f MS/s\n', ...
            numel(x), numel(x)/fs*1e3, fs/1e6);
    fprintf('  reference : generated, length %d, root %d\n', N, zc_root);
    fprintf('  rms       : %.6f   (0.00061 = noise, 0.028 = ZC signal)\n', ...
            sqrt(mean(abs(x).^2)));

    if ~strcmp(getfield_default(meta, 'overflows', '0'), '0')
        fprintf('  WARNING: capture reports %s overflows -- it has gaps\n', ...
                meta.overflows);
    end

    % Correlate over a couple of hundred repetitions. Eight is enough to find
    % the sequence, but the carrier-offset estimate below averages the phase
    % step between repetitions and needs more of them to be worth trusting.
    win = min(numel(x), 200*N);
    seg = x(1:win);

    if numel(seg) < N
        fprintf('  capture is shorter than the sequence\n');
        return
    end

    % Matched filter: conv with the time-reversed conjugate is the same as
    % sliding correlation, and is far faster than a loop.
    c = conv(seg, conj(flipud(zc)), 'valid');
    m = abs(c);

    [peak, idx] = max(m);
    pmr = peak / mean(m);

    fprintf('  peak/mean : %.1f at lag %d\n', pmr, idx-1);

    if pmr < 5
        fprintf('\n  NO detection -- that is the noise floor.\n');
        fprintf('  Check the transmitter is running and TX gain is 80.\n');
        return
    end

    % Peak spacing is the real test of a detection.
    thr     = mean(m) + 0.5*(peak - mean(m));
    peaks   = find(m > thr);
    gaps    = diff(peaks);
    gaps    = gaps(gaps > N/2);
    spacing = median(gaps);

    fprintf('  peaks     : %d above threshold\n', numel(peaks));
    fprintf('  spacing   : %.0f samples (expect %d)\n', spacing, N);

    % Carrier offset between the two radios, from the phase advance between
    % consecutive repetitions of the sequence.
    reps = floor((numel(c) - idx) / N);
    if reps >= 2
        k    = idx + (0:reps-1)*N;
        step = mean(c(k(2:end)) .* conj(c(k(1:end-1))));
        cfo  = angle(step) / (2*pi*N/fs);
        fprintf('  CFO       : %+.1f Hz over %d repetitions\n', cfo, reps);
    end

    if abs(spacing - N) <= 2
        fprintf('\n  DETECTED, spacing matches the sequence length\n');
    else
        fprintf('\n  peaks found but spacing is wrong -- see NOTES section 3.3\n');
    end

    figure('Name', 'ZC capture', 'NumberTitle', 'off');

    subplot(2,1,1);
    plot((0:win-1)/fs*1e3, abs(seg));
    xlabel('ms'); ylabel('|x|'); title('Capture envelope'); grid on;

    subplot(2,1,2);
    plot(0:numel(m)-1, m); hold on;
    plot(peaks-1, m(peaks), 'ro');
    xlabel('lag (samples)'); ylabel('|correlation|');
    title(sprintf('Matched filter, peak/mean %.1f', pmr)); grid on;
end


function x = read_fc32(path)
    f = fopen(path, 'r');
    if f < 0, error('cannot open %s', path); end
    raw = fread(f, Inf, 'float32');
    fclose(f);
    x = complex(raw(1:2:end), raw(2:2:end));
end


function meta = read_sidecar(path)
    meta = struct();
    f = fopen([path '.txt'], 'r');
    if f < 0, return; end
    while true
        line = fgetl(f);
        if ~ischar(line), break; end
        parts = strsplit(strtrim(line), '=');
        if numel(parts) == 2
            meta.(matlab.lang.makeValidName(parts{1})) = parts{2};
        end
    end
    fclose(f);
end


function v = getfield_default(s, name, default)
    if isfield(s, name)
        v = str2double(s.(name));
        if isnan(v), v = s.(name); end
    else
        v = default;
    end
end
