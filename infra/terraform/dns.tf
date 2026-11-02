# Records in the existing napple.lat zone (looked up by id or name, never managed): the game servers, plus the GitHub Pages landing page.
# GitHub Pages addresses: https://docs.github.com/pages/configuring-a-custom-domain-for-your-github-pages-site/managing-a-custom-domain-for-your-github-pages-site
data "aws_route53_zone" "main" {
  zone_id      = var.zone_id != "" ? var.zone_id : null
  name         = var.zone_id == "" ? var.domain : null
  private_zone = false
}

locals {
  pages_target = "${lower(var.github_owner)}.github.io"
  pages_ipv4   = ["185.199.108.153", "185.199.109.153", "185.199.110.153", "185.199.111.153"]
  pages_ipv6   = ["2606:50c0:8000::153", "2606:50c0:8001::153", "2606:50c0:8002::153", "2606:50c0:8003::153"]
  server_fqdn  = "${var.server_subdomain}.${var.domain}"
}

resource "aws_route53_record" "server" {
  zone_id         = data.aws_route53_zone.main.zone_id
  name            = local.server_fqdn
  type            = "A"
  ttl             = var.dns_ttl
  records         = [aws_eip.servers.public_ip]
  allow_overwrite = var.dns_allow_overwrite
}

resource "aws_route53_record" "apex_a" {
  count           = var.manage_pages_dns ? 1 : 0
  zone_id         = data.aws_route53_zone.main.zone_id
  name            = var.domain
  type            = "A"
  ttl             = var.dns_ttl
  records         = local.pages_ipv4
  allow_overwrite = var.dns_allow_overwrite
}

resource "aws_route53_record" "apex_aaaa" {
  count           = var.manage_pages_dns ? 1 : 0
  zone_id         = data.aws_route53_zone.main.zone_id
  name            = var.domain
  type            = "AAAA"
  ttl             = var.dns_ttl
  records         = local.pages_ipv6
  allow_overwrite = var.dns_allow_overwrite
}

resource "aws_route53_record" "www" {
  count           = var.manage_pages_dns ? 1 : 0
  zone_id         = data.aws_route53_zone.main.zone_id
  name            = "www.${var.domain}"
  type            = "CNAME"
  ttl             = var.dns_ttl
  records         = [local.pages_target]
  allow_overwrite = var.dns_allow_overwrite
}

# Every other name (app.napple.lat included: deliberately not built) goes to GitHub Pages. An explicit record (pretend, www) always wins over it.
resource "aws_route53_record" "wildcard" {
  count           = var.manage_pages_dns && var.wildcard_to_pages ? 1 : 0
  zone_id         = data.aws_route53_zone.main.zone_id
  name            = "*.${var.domain}"
  type            = "CNAME"
  ttl             = var.dns_ttl
  records         = [local.pages_target]
  allow_overwrite = var.dns_allow_overwrite
}

resource "aws_route53_record" "pages_verification" {
  count           = var.github_pages_verification_txt != "" ? 1 : 0
  zone_id         = data.aws_route53_zone.main.zone_id
  name            = "_github-pages-challenge-${lower(var.github_owner)}.${var.domain}"
  type            = "TXT"
  ttl             = var.dns_ttl
  records         = [var.github_pages_verification_txt]
  allow_overwrite = var.dns_allow_overwrite
}
