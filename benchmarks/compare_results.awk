BEGIN {
    FS = ","
}

FNR == 1 {
    next
}

{
    key = $3 SUBSEP $4
    if (!(key in seen)) {
        seen[key] = 1
        order[++case_count] = key
        case_name[key] = $3
        payload[key] = $4
    }
    total_operations[$1, key] += $5
    total_seconds[$1, key] += $6
    total_bytes[$1, key] += $4 * $5
}

END {
    printf "%-16s %8s %16s %16s %10s %14s %14s\n", "case", "bytes", \
           "main ops/s", "refactor ops/s", "delta", "main MiB/s", "ref MiB/s"
    for (i = 1; i <= case_count; i++) {
        key = order[i]
        main_ops = total_operations["main", key] / total_seconds["main", key]
        ref_ops = total_operations["refactor", key] / total_seconds["refactor", key]
        main_mib = total_bytes["main", key] \
            / (total_seconds["main", key] * 1024 * 1024)
        ref_mib = total_bytes["refactor", key] \
            / (total_seconds["refactor", key] * 1024 * 1024)
        delta = main_ops == 0 ? 0 : (ref_ops / main_ops - 1) * 100
        printf "%-16s %8d %16.1f %16.1f %+9.2f%% %14.2f %14.2f\n", \
               case_name[key], payload[key], main_ops, ref_ops, delta, main_mib, ref_mib
    }
}
